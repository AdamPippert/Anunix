/*
 * test_posix.c — Tests for POSIX compatibility shim.
 */

#include <anx/types.h>
#include <anx/posix.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/uuid.h>

int test_posix(void)
{
	int fd, ret;
	char wbuf[] = "hello, anunix";
	char rbuf[64];
	struct anx_posix_stat st;

	anx_objstore_init();
	anx_ns_init();
	anx_posix_init();
	anx_cell_store_init();

	/* Open with O_CREAT creates a new file */
	fd = anx_posix_open("/test.txt", ANX_O_RDWR | ANX_O_CREAT);
	if (fd < 0)
		return -1;

	/* Write data */
	{
		ssize_t written;

		written = anx_posix_write(fd, wbuf, anx_strlen(wbuf));
		if (written != (ssize_t)anx_strlen(wbuf))
			return -2;
	}

	/* Close and reopen for reading */
	ret = anx_posix_close(fd);
	if (ret != ANX_OK)
		return -3;

	fd = anx_posix_open("/test.txt", ANX_O_RDONLY);
	if (fd < 0)
		return -4;

	/* Read data back */
	{
		ssize_t nread;

		anx_memset(rbuf, 0, sizeof(rbuf));
		nread = anx_posix_read(fd, rbuf, anx_strlen(wbuf));
		if (nread != (ssize_t)anx_strlen(wbuf))
			return -5;
		if (anx_strcmp(rbuf, wbuf) != 0)
			return -6;
	}

	anx_posix_close(fd);

	/* Stat the file */
	ret = anx_posix_stat("/test.txt", &st);
	if (ret != ANX_OK)
		return -7;
	if (st.size != anx_strlen(wbuf))
		return -8;

	/* Stat a nonexistent file */
	ret = anx_posix_stat("/nonexistent", &st);
	if (ret != ANX_ENOENT)
		return -9;

	/* Open nonexistent without O_CREAT fails */
	fd = anx_posix_open("/no_such_file", ANX_O_RDONLY);
	if (fd >= 0)
		return -10;

	/* Fork creates a child cell */
	{
		anx_cid_t child_cid;

		child_cid = anx_posix_fork();
		if (anx_uuid_is_nil(&child_cid))
			return -11;

		/* Wait for child (stub: returns immediately) */
		int status = 0;

		ret = anx_posix_wait(child_cid, &status);
		if (ret != ANX_OK)
			return -12;
	}

	return 0;
}
