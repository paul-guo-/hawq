package com.pivotal.pxf.accessors;

import java.io.IOException;
import java.util.LinkedList;
import java.util.ListIterator;

import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.mapred.FileInputFormat;
import org.apache.hadoop.mapred.InputSplit;
import org.apache.hadoop.mapred.JobConf;
import org.apache.hadoop.mapred.RecordReader;

import com.pivotal.pxf.format.OneRow;
import com.pivotal.pxf.utilities.HdfsUtilities;
import com.pivotal.pxf.utilities.InputData;
import com.pivotal.pxf.utilities.Plugin;

/*
 * Implementation of Accessor for accessing a splittable data source
 * - it means that HDFS will divide the file into splits based on an internal algorithm (by default, the 
 * the block size 64 MB is also the split size).  
 */
public abstract class HdfsSplittableDataAccessor extends Plugin implements IReadAccessor
{
	private   LinkedList<InputSplit> segSplits = null;
	private   InputSplit currSplit = null;
	protected Configuration conf = null;
	protected RecordReader<Object, Object> reader = null;
	protected FileInputFormat<?, ?> fformat = null;
	protected ListIterator<InputSplit> iter = null;
	protected JobConf jobConf = null;
	protected Object key, data;

	/*
	 * C'tor
	 */ 
	public HdfsSplittableDataAccessor(InputData input, 
						          FileInputFormat<?, ?> inFormat) throws Exception
	{
		super(input);
		fformat = inFormat;

		// 1. Load Hadoop configuration defined in $HADOOP_HOME/conf/*.xml files
		conf = new Configuration();

		// 2. variable required for the splits iteration logic 
		jobConf = new JobConf(conf, HdfsSplittableDataAccessor.class);
	}

	/*
	 * openForRead
	 * Fetches the first split (relevant to this segment) in the file, using
	 * the splittable API - InputFormat
	 */	
	public boolean openForRead() throws Exception
	{
		// 1. get the list of all splits the input file has
        FileInputFormat.setInputPaths(jobConf, new Path(inputData.path()));

        /*
		 * We ask for 1 split, but this doesn't mean that we are always going to get 1 split
		 * The number of splits depends on the data size. What we assured here is that always,
		 * the split size will be equal to the block size. To understand this, one should look
		 * at the implementation of FileInputFormat.getSplits()
		 */
		InputSplit[] splits = fformat.getSplits(jobConf, 1);
		int actual_num_of_splits = splits.length;

		// 2. from all the splits choose only those that correspond to this segment id
		segSplits = new LinkedList<InputSplit>();

		int needed_split_idx = inputData.getDataFragment();

		/*
		 * Testing for the case where between the time of the GP Master input
		 * data retrieval and now, the file was deleted or replaced by a smaller file.
		 * This is an extreme case which shouldn't happen - but we want to make sure
		 */
		if ((needed_split_idx != InputData.INVALID_SPLIT_IDX) && (needed_split_idx < actual_num_of_splits))
			segSplits.add(splits[needed_split_idx]);			
		
		
		// 3. Initialize record reader based on current split
		iter = segSplits.listIterator(0);
		return getNextSplit();	
	}
	
	/*
	 * Specialized accessors will override this method and implement their own recordReader
	 */
	abstract protected Object getReader(JobConf jobConf, InputSplit split) throws IOException;

	/*
	 * getNextSplit
	 * Sets the current split and initializes a RecordReader who feeds from the split
	 */
	@SuppressWarnings(value = "unchecked")
	protected boolean getNextSplit() throws IOException
	{
		if (!iter.hasNext())
			return false;
		
		currSplit = iter.next();
		reader = (RecordReader<Object, Object>)getReader(jobConf, currSplit);	 // approach the specialized class override function	
		key = reader.createKey(); 
		data = reader.createValue();
		return true;
	}

	/*
	 * readNextObject
	 * Fetches one record from the  file. The record is returned as a Java object.
	 */		
	public OneRow readNextObject() throws IOException
	{
		if (!reader.next(key, data)) // if there is one more record in the current split
		{
			if (getNextSplit()) // the current split is exhausted. try to move to the next split.
			{
				if (!reader.next(key, data)) // read the first record of the new split
					return null; // make sure we return nulls 
			}
			else
				return null; // make sure we return nulls 
		}
		
		/*
		 * if neither condition was met, it means we already read all the records in all the splits, and
		 * in this call record variable was not set, so we return null and thus we are signaling end of 
		 * records sequence
		*/
		return new OneRow(key, data);
	}

	/*
	 * closeForRead
	 * When user finished reading the file, it closes the RecordReader
	 */		
	public void closeForRead() throws Exception
	{
		if (reader != null)
			reader.close();
	}
	
	@Override
	public boolean isThreadSafe() {
		return HdfsUtilities.isThreadSafe(inputData);
	}
	
}
