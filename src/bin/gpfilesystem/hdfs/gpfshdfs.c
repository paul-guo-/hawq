/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*-------------------------------------------------------------------------
 *
 * gpfshdfs.c
 *
 * This file provide a HDFS filesystem interface to GPDB. It uses libhdfs
 * to communicate with HDFS.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>

#include "fmgr.h"
#include "funcapi.h"
#include "access/extprotocol.h"
#include "catalog/pg_proc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "access/fileam.h"
#include "catalog/pg_exttable.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "storage/filesystem.h"
#include "hdfs/hdfs.h"

/* Do the module magic dance */
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(gpfs_hdfs_connect);
PG_FUNCTION_INFO_V1(gpfs_hdfs_disconnect);
PG_FUNCTION_INFO_V1(gpfs_hdfs_openfile);
PG_FUNCTION_INFO_V1(gpfs_hdfs_sync);
PG_FUNCTION_INFO_V1(gpfs_hdfs_closefile);
PG_FUNCTION_INFO_V1(gpfs_hdfs_createdirectory);
PG_FUNCTION_INFO_V1(gpfs_hdfs_delete);
PG_FUNCTION_INFO_V1(gpfs_hdfs_chmod);

PG_FUNCTION_INFO_V1(gpfs_hdfs_read);
PG_FUNCTION_INFO_V1(gpfs_hdfs_write);
PG_FUNCTION_INFO_V1(gpfs_hdfs_seek);
PG_FUNCTION_INFO_V1(gpfs_hdfs_tell);

PG_FUNCTION_INFO_V1(gpfs_hdfs_truncate);

PG_FUNCTION_INFO_V1(gpfs_hdfs_getpathinfo);
PG_FUNCTION_INFO_V1(gpfs_hdfs_freefileinfo);

Datum gpfs_hdfs_connect(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_disconnect(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_openfile(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_sync(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_closefile(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_createdirectory(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_delete(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_chmod(PG_FUNCTION_ARGS);

Datum gpfs_hdfs_read(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_write(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_seek(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_tell(PG_FUNCTION_ARGS);

Datum gpfs_hdfs_truncate(PG_FUNCTION_ARGS);

Datum gpfs_hdfs_getpathinfo(PG_FUNCTION_ARGS);
Datum gpfs_hdfs_freefileinfo(PG_FUNCTION_ARGS);

#ifdef HDFS_FD_HASH
struct hdfs_fd_key {
	hdfsFS hdfs;
	hdfsFile hFile;
	char *path;
	int flag;
}

#define MAX_HDFS_FD 128
static struct hdfs_fd_key *fd_list[MAX_HDFS_FD];
static unsigned int hdfs_fd_next = 0;
static unsigned int hdfs_fd_len = 0;

/* Just compare via path & flag */
bool
match_fd(struct hdfs_fd_key *key1, struct hdfs_fd_key *key2)
{
	if (strcmp(key1->path, key2->path) == 0 &&
		key1->flag = key2->flag)
			return true;

	return false;
}

/* < 0: not found. other: index */
int
find_fd(struct hdfs_fd_key *key)
{
	int i;

	for (i = 0; i < hdfs_fd_len; i++) {
		if (match_fd(key, fd_list[i]))
			return i;
	}

	return -1;
}

/* Append the key via {path&flag} */
int
insert_fd_if_needed(char *path, int flag)
{
	int idx;
	struct hdfs_fd_key *old_key;

	idx = find_fd(path, flag);

	if (idx >= 0) {
		free(path);
		return;
	}

	key = malloc(sizeof(hdfs_fd_key));
	key->path = path;
	key->flag = flag;
	hFile = hdfsOpenFile(hdfs, path, flags, bufferSize, rep, blocksize);

	/* Not found. */
	if (hdfs_fd_len < MAX_HDFS_FD) {
		/* append */
		fd_list[hdfs_fd_next] = key;
		hdfs_fd_next++;
		hdfs_fd_len++;
	} else {
		/* replace */
		hdfs_fd_next = hdfs_fd_next % MAX_HDFS_FD;

		old_key = fd_list[hdfs_fd_next];
		hdfsCloseFile(old_key->hdfs, old_key->hFile);
		free(old_key->path);
		free(old_key);

		fd_list[hdfs_fd_next] = key;
		hdfs_fd_next++;
	}
}

/* code logic:
 * open():
 *       if {hdfs, path, flag} in list
 *          Move the key to the list end;
 *          seek(0) if needed, and return the key;
 *       else
 *          Open it and append it into list;
 * seek():
 *       do nothing.
 * close():
 *       do nothing and append it into list.
 * open():
 */

#endif

/*
 * hdfsFS hdfsConnect(const char * host, uint16_t port);
 */
Datum
gpfs_hdfs_connect(PG_FUNCTION_ARGS)
{
	char *host = NULL;
	int port = 0;
	hdfsFS hdfs = NULL;
	int retval = 0;
	void *token = NULL;
	char *ccname = NULL;

	struct hdfsBuilder *builder;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_connect outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	host = FSYS_UDF_GET_HOST(fcinfo);
	port = FSYS_UDF_GET_PORT(fcinfo);
	token = FSYS_UDF_GET_TOKEN(fcinfo);
	ccname = FSYS_UDF_GET_CCNAME(fcinfo);

	if (NULL == host) {
		elog(WARNING, "get host invalid in gpfs_hdfs_connect");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (port < 0) {
		elog(WARNING, "get port invalid in gpfs_hdfs_connect: %d", port);
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	builder = hdfsNewBuilder();

	if (NULL == builder) {
		elog(WARNING, "failed to create hdfs connection builder in gpfs_hdfs_connect");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfsBuilderSetNameNode(builder, host);
	if (port != 0)
		hdfsBuilderSetNameNodePort(builder, port);

	if (token) {
		hdfsBuilderSetToken(builder, token);
	}

	if (ccname) {
		hdfsBuilderSetKerbTicketCachePath(builder, ccname);
	}

	hdfsBuilderSetForceNewInstance(builder);

	hdfs = hdfsBuilderConnect(builder);
	hdfsFreeBuilder(builder);

	if (NULL == hdfs) {
		retval = -1;
	}
	FSYS_UDF_SET_HDFS(fcinfo, hdfs);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsDisconnect(hdfsFS fileSystem);
 */
Datum
gpfs_hdfs_disconnect(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_openfile outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);

	if (NULL == hdfs) {
		elog(WARNING, "get hdfs invalid in gpfs_hdfs_openfile");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsDisconnect(hdfs);

	PG_RETURN_INT32(retval);
}

/*
 * hdfsFile hdfsOpenFile(hdfsFS fileSystem, const char * path, int flags, int bufferSize, short replication, int64_t blocksize);
 */
Datum
gpfs_hdfs_openfile(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	char *path = NULL;
	int flags = 0;
	int bufferSize = 0;
	short rep = 0;
	int64_t blocksize = 0;
	hdfsFile hFile = NULL;
	int numRetry = 300;
	long sleepTime = 0; //micro seconds
	const long maxSleep = 1 * 1000 * 1000; //1 seconds

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_openfile outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	path = FSYS_UDF_GET_PATH(fcinfo);
	flags = FSYS_UDF_GET_FILEFLAGS(fcinfo);
	bufferSize = FSYS_UDF_GET_FILEBUFSIZE(fcinfo);
	rep = FSYS_UDF_GET_FILEREP(fcinfo);
	blocksize = FSYS_UDF_GET_FILEBLKSIZE(fcinfo);

//paul
//elog(WARNING, "hdfs open: path: %s, flags: %o, bufferSize: %d, rep: %d, blocksize: %d", path, flags, bufferSize, rep, blocksize);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfs invalid in gpfs_hdfs_openfile");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == path || '\0' == *path) {
		elog(WARNING, "get path invalid in gpfs_hdfs_openfile");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (bufferSize < 0 || rep < 0 || blocksize < 0) {
		elog(WARNING, "get param error in gpfs_hdfs_openfile: bufferSize[%d], rep[%d], blocksize["INT64_FORMAT"]",
			 bufferSize, rep, blocksize);
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

//paul
//elog(WARNING, "%s() calls. path: %s, flags: %o\n", __func__, path, flags);
	do {
		if (sleepTime > 0) {
			pg_usleep(sleepTime);
		}

struct timeval start, end;
//paul
gettimeofday(&start, NULL);
		hFile = hdfsOpenFile(hdfs, path, flags, bufferSize, rep, blocksize);
gettimeofday(&end, NULL);
//elog(WARNING, "  hdfs open time: %d s + %d us\n", (int)(end.tv_sec-start.tv_sec), (int)(end.tv_usec-start.tv_usec));
		sleepTime = sleepTime * 2 + 10000;
		sleepTime = sleepTime < maxSleep ? sleepTime : maxSleep;
	} while (--numRetry > 0 && hFile == NULL && errno == EBUSY);
//elog(WARNING, "  hdfs open close, hFile: %p\n", hFile);

	if (NULL == hFile) {
		retval = -1;
	}
	FSYS_UDF_SET_HFILE(fcinfo, hFile);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsReopen(hdfsFS fileSystem, hdfsFile file);
 */
Datum
gpfs_hdfs_sync(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	hdfsFile hFile = NULL;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_sync outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	hFile = FSYS_UDF_GET_HFILE(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_sync");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == hFile) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_sync");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsSync(hdfs, hFile);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsCloseFile(hdfsFS fileSystem, hdfsFile file);
 */
Datum
gpfs_hdfs_closefile(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	hdfsFile hFile = NULL;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_closefile outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	hFile = FSYS_UDF_GET_HFILE(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_closefile");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == hFile) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_closefile");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

//elog(WARNING, "%s() calls. hFile: %p", __func__, hFile);

	retval = hdfsCloseFile(hdfs, hFile);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsCreateDirectory(hdfsFS fileSystem, const char * path);
 */
Datum
gpfs_hdfs_createdirectory(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	char *path = NULL;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_createdirectory outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	path = FSYS_UDF_GET_PATH(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_createdirectory");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == path) {
		elog(WARNING, "get path invalid in gpfs_hdfs_createdirectory");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsCreateDirectory(hdfs, path);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsDelete(hdfsFS fileSystem, const char * path, int recursive);
 */
Datum
gpfs_hdfs_delete(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	char *path = NULL;
	int recursive = 0;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_delete outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	path = FSYS_UDF_GET_PATH(fcinfo);
	recursive = FSYS_UDF_GET_RECURSIVE(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_delete");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == path) {
		elog(WARNING, "get path invalid in gpfs_hdfs_delete");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsDelete(hdfs, path, recursive);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsChmod(hdfsFS fileSystem, const char * path, short mode);
 */
Datum
gpfs_hdfs_chmod(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	char *path = NULL;
	short mod = 0;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_chmod outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	path = FSYS_UDF_GET_PATH(fcinfo);
	mod = FSYS_UDF_GET_MODE(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_chmod");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == path) {
		elog(WARNING, "get path invalid in gpfs_hdfs_chmod");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsChmod(hdfs, path, mod);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsRead(hdfsFS fileSystem, hdfsFile file, void * buffer, int length);
 */
Datum
gpfs_hdfs_read(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	hdfsFile hFile = NULL;
	char *buf = NULL;
	int length = 0;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_read outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	hFile = FSYS_UDF_GET_HFILE(fcinfo);
	buf = FSYS_UDF_GET_DATABUF(fcinfo);
	length = FSYS_UDF_GET_BUFLEN(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_read");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == hFile) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_read");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == buf) {
		elog(WARNING, "get buffer invalid in gpfs_hdfs_read");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (length < 0) { /* TODO liugd: or <= 0 ? */
		elog(WARNING, "get length[%d] invalid in gpfs_hdfs_read", length);
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
//paul
struct timeval start, end;
gettimeofday(&start, NULL);

	retval = hdfsRead(hdfs, hFile, buf, length);

gettimeofday(&end, NULL);
elog(WARNING, "  hdfs read time: %d s + %d us\n", (int)(end.tv_sec-start.tv_sec), (int)(end.tv_usec-start.tv_usec));

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsWrite(hdfsFS fileSystem, hdfsFile file, const void * buffer, int length);
 */
Datum
gpfs_hdfs_write(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	hdfsFile hFile = NULL;
	char *buf = NULL;
	int length = 0;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_write outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
//elog(WARNING, "%s() calls", __func__);
	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	hFile = FSYS_UDF_GET_HFILE(fcinfo);
	buf = FSYS_UDF_GET_DATABUF(fcinfo);
	length = FSYS_UDF_GET_BUFLEN(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_write");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == hFile) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_write");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == buf) {
		elog(WARNING, "get buffer invalid in gpfs_hdfs_write");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (length < 0) { /* TODO liugd: or <= 0 ? */
		elog(WARNING, "get length[%d] invalid in gpfs_hdfs_write", length);
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsWrite(hdfs, hFile, buf, length);

	PG_RETURN_INT32(retval);
}

/*
 * int hdfsSeek(hdfsFS fileSystem, hdfsFile file, int64_t desiredPos);
 */
Datum
gpfs_hdfs_seek(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	hdfsFile hFile = NULL;
	int64_t pos = 0;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_seek outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	hFile = FSYS_UDF_GET_HFILE(fcinfo);
	pos = FSYS_UDF_GET_POS(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_seek");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}
	if (NULL == hFile) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_seek");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}
	if (pos < 0) {
		elog(WARNING, "get pos["INT64_FORMAT"] invalid in gpfs_hdfs_seek", pos);
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}

//elog(WARNING, "%s() calls", __func__);
struct timeval start, end;
gettimeofday(&start, NULL);

	retval = hdfsSeek(hdfs, hFile, pos);

gettimeofday(&end, NULL);
//elog(WARNING, "  hdfs seek time: %d s + %d us\n", (int)(end.tv_sec-start.tv_sec), (int)(end.tv_usec-start.tv_usec));
	PG_RETURN_INT64(retval);
}

/*
 * int64_t hdfsTell(hdfsFS fileSystem, hdfsFile file);
 */
Datum
gpfs_hdfs_tell(PG_FUNCTION_ARGS)
{
	int64_t retval = 0;
	hdfsFS hdfs = NULL;
	hdfsFile hFile = NULL;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_tell outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	hFile = FSYS_UDF_GET_HFILE(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_tell");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}
	if (NULL == hFile) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_tell");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}

	retval = hdfsTell(hdfs, hFile);

	PG_RETURN_INT64(retval);
}

/*
 * int hdfsTruncate(hdfsFS fileSystem, const char * path, int64_t size);
 */
Datum
gpfs_hdfs_truncate(PG_FUNCTION_ARGS)
{
	int retval = 0;
	hdfsFS hdfs = NULL;
	char *path = NULL;
	int64_t pos = 0;
	int shouldWait;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_tell outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	path = FSYS_UDF_GET_PATH(fcinfo);
	pos = FSYS_UDF_GET_POS(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_truncate");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == path) {
		elog(WARNING, "get hdfsFile invalid in gpfs_hdfs_truncate");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (pos < 0) {
		elog(WARNING, "get pos["INT64_FORMAT"] invalid in gpfs_hdfs_truncate", pos);
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	retval = hdfsTruncate(hdfs, path, pos, &shouldWait);

	PG_RETURN_INT32(retval);
}

/*
 * void hdfsLogLevel(LogLevel level);
 * void hdfsLogOutput(FILE * file);
 */

/*
 * int hdfsSetReplication(hdfsFS fileSystem, const char * path, int16_t replication);
 */

/*
 * hdfsFileInfo * hdfsGetPathInfo(hdfsFS fileSystem, const char * path);
 */
Datum
gpfs_hdfs_getpathinfo(PG_FUNCTION_ARGS)
{
	int64_t retval = 0;
	hdfsFS hdfs = NULL;
	char *path = NULL;
	hdfsFileInfo *fileinfo = NULL;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_tell outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	hdfs = FSYS_UDF_GET_HDFS(fcinfo);
	path = FSYS_UDF_GET_PATH(fcinfo);
	if (NULL == hdfs) {
		elog(WARNING, "get hdfsFS invalid in gpfs_hdfs_getpathinfo");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}
	if (NULL == path) {
		elog(WARNING, "get file path invalid in gpfs_hdfs_getpathinfo");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT32(retval);
	}

	fileinfo = hdfsGetPathInfo(hdfs, path);
	if (NULL == fileinfo) {
		retval = -1;
	}

	FSYS_UDF_SET_FILEINFO(fcinfo, fileinfo);

	PG_RETURN_INT64(retval);
}

Datum
gpfs_hdfs_freefileinfo(PG_FUNCTION_ARGS)
{
	int64_t retval = 0;
	hdfsFileInfo *fileinfo = NULL;
	int numEntries = 0;

	/* Must be called via the filesystem manager */
	if (!CALLED_AS_GPFILESYSTEM(fcinfo)) {
		elog(WARNING, "cannot execute gpfs_hdfs_tell outside filesystem manager");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}

	fileinfo = FSYS_UDF_GET_FILEINFO(fcinfo);
	numEntries = FSYS_UDF_GET_FILEINFONUM(fcinfo);
	if (NULL == fileinfo) {
		elog(WARNING, "get hdfsFileInfo invalid in gpfs_hdfs_freefileinfo");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}
	if (numEntries < 0) {
		elog(WARNING, "get hdfsFileInfo numEntries invalid in gpfs_hdfs_freefileinfo");
		retval = -1;
		errno = EINVAL;
		PG_RETURN_INT64(retval);
	}

	hdfsFreeFileInfo(fileinfo, numEntries);

	PG_RETURN_INT64(retval);
}
