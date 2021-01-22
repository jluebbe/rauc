#include <locale.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "dm.h"
#include "verity_hash.h"
#include "mount.h"
#include "utils.h"

#include "common.h"


typedef struct {
	gchar *tmpdir;
} DMFixture;

typedef struct {
	off_t data_size;
	off_t combined_size;
} DMData;

static void dm_fixture_set_up(DMFixture *fixture,
		gconstpointer user_data)
{
	fixture->tmpdir = g_dir_make_tmp("rauc-XXXXXX", NULL);
	g_assert_nonnull(fixture->tmpdir);
	g_print("dm tmpdir: %s\n", fixture->tmpdir);
}

static void dm_fixture_tear_down(DMFixture *fixture,
		gconstpointer user_data)
{
	g_assert_true(rm_tree(fixture->tmpdir, NULL));
	g_free(fixture->tmpdir);
}

static void flip_bits_fd(int fd, off_t offset, guint8 mask)
{
	guint8 buf;
	g_assert_cmpint(fd, >, 0);
	g_assert_cmphex(mask, !=, 0);
	g_assert(pread(fd, &buf, 1, offset) == 1);
	buf = buf ^ mask;
	g_assert(pwrite(fd, &buf, 1, offset) == 1);
}

static void flip_bits_filename(gchar *filename, off_t offset, guint8 mask)
{
	int fd = g_open(filename, O_RDWR|O_CLOEXEC, 0);
	g_assert_cmpint(fd, >, 0);
	flip_bits_fd(fd, offset, mask);
	g_assert(fsync(fd) == 0);
	g_close(fd, NULL);
}

static void drop_caches(void)
{
	int fd = g_open("/proc/sys/vm/drop_caches", O_WRONLY|O_CLOEXEC, 0);
	g_assert_cmpint(fd, >, 0);
	g_assert_true(write(fd, "1", 1) == 1);
	g_close(fd, NULL);
}

static gint readable_sectors(int fd, guint8 *content, off_t size)
{
	guint8 buf[4096];
	gint sectors = 0;

	lseek(fd, 0, SEEK_SET);

	for (guint sector = 0;; sector++) {
		ssize_t r = pread(fd, buf, sizeof(buf), sector*sizeof(buf));

		if (r == 0) {
			break;
		} else if (r < 0) {
			g_test_message("%s: error while reading sector %u: %s", G_STRFUNC, sector, g_strerror(errno));
		} else if (r != sizeof(buf)) {
			g_test_message("%s: read only %ld bytes in sector %u", G_STRFUNC, r, sector);
			g_test_fail();
			return -1;
		} else {
			g_assert((sector*4096 + r) <= size);
			if (memcmp(buf, &content[sector * 4096], 4096) != 0) {
				g_test_message("%s: difference found in sector %u", G_STRFUNC, sector);
				g_test_fail();
				return -1;
			} else {
				sectors++;
			}
		}
	}
	return sectors;
}

static int open_loop_verity(int bundlefd, off_t loop_size, off_t data_size, gchar *root_digest, gchar *salt, GError **error)
{
	GError *ierror = NULL;
	gboolean res;
	g_autoptr(RaucDMVerity) dm_verity = NULL;
	int loopfd = -1;
	gchar *loopname = NULL;
	int fd = -1;

	g_assert_cmpint(bundlefd, >, 0);

	res = r_setup_loop(bundlefd, &loopfd, &loopname, loop_size, &ierror);
	g_assert_no_error(ierror);
	g_assert_true(res);
	g_assert_nonnull(loopname);

	dm_verity = new_dm_verity();
	dm_verity->lower_dev = g_strdup(loopname);
	dm_verity->data_size = data_size;
	dm_verity->root_digest = g_strdup(root_digest);
	dm_verity->salt = g_strdup(salt);

	res = setup_dm_verity(dm_verity, &ierror);
	g_close(loopfd, NULL);
	if (!res) {
		g_propagate_error(error, ierror);
		goto out;
	}

	g_assert_nonnull(dm_verity->upper_dev);

	fd = g_open(dm_verity->upper_dev, O_RDONLY|O_CLOEXEC, 0);
	g_assert_cmpint(fd, >, 0);

	res = remove_dm_verity(dm_verity, TRUE, &ierror);
	g_assert_no_error(ierror);
	g_assert_true(res);

out:
	return fd;
}

static void dm_verity_simple_test(void)
{
	GError *error = NULL;
	gboolean res;
	g_autoptr(RaucDMVerity) dm_verity = NULL;
	int datafd = -1;
	int loopfd = -1;
	gchar *loopname = NULL;
	int fd = -1;
	guchar buf[4096];

	/* needs to run as root */
	if (!test_running_as_root())
		return;

	datafd = g_open("test/dummy.verity", O_RDONLY|O_CLOEXEC, 0);
	g_assert_cmpint(datafd, >, 0);

	res = r_setup_loop(datafd, &loopfd, &loopname, 4096*132, &error);
	g_assert_no_error(error);
	g_assert_true(res);
	g_assert_nonnull(loopname);
	g_close(datafd, NULL);

	dm_verity = new_dm_verity();
	dm_verity->lower_dev = g_strdup(loopname);
	dm_verity->data_size = 4096*129;
	dm_verity->root_digest = g_strdup("3049cbffaa49c6dc12e9cd1dd4604ef5a290e3d13b379c5a50d356e68423de23");
	dm_verity->salt = g_strdup("799ea94008bbdc6555d7895d1b647e2abfd213171f0e8b670e1da951406f4691");

	res = setup_dm_verity(dm_verity, &error);
	g_assert_no_error(error);
	g_assert_true(res);
	g_close(loopfd, NULL);

	g_assert_nonnull(dm_verity->upper_dev);

	fd = g_open(dm_verity->upper_dev, O_RDONLY|O_CLOEXEC, 0);
	g_assert_cmpint(fd, >, 0);

	res = remove_dm_verity(dm_verity, TRUE, &error);
	g_assert_no_error(error);
	g_assert_true(res);

	for (int i = 0; i<129; i++) {
		int r = read(fd, buf, sizeof(buf));
		g_assert_cmpint(r, ==, 4096);
		g_assert_cmpint(buf[0], ==, 0);
		g_assert_cmpint(buf[1], ==, 0);
		g_assert_cmpint(buf[2], ==, 0);
		g_assert_cmpint(buf[3], ==, i);
	}

	g_close(fd, NULL);
}

static void verity_hash_test(void)
{
	int ret, bundlefd;
	g_autofree guint8 *root_hash = r_hex_decode("3049cbffaa49c6dc12e9cd1dd4604ef5a290e3d13b379c5a50d356e68423de23", 32);
	g_autofree guint8 *salt = r_hex_decode("799ea94008bbdc6555d7895d1b647e2abfd213171f0e8b670e1da951406f4691", 32);

	/* needs to run as root */
	if (!test_running_as_root())
		return;

	bundlefd = g_open("test/dummy.verity", O_RDONLY);
	g_assert_cmpint(bundlefd, >, 0);

	ret = verity_create_or_verify_hash(1, bundlefd, 129, NULL, root_hash, salt);
	g_assert_cmpint(ret, ==, 0);

	g_close(bundlefd, NULL);
}

static void verity_hash_create(DMFixture *fixture,
		gconstpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autofree guint8 *content = NULL;
	const DMData *dm_data = user_data;
	int ret, bundlefd;
	guint8 root_hash[32] = {0};
	g_autofree gchar *filename = NULL;
	g_autofree gchar *root_hash_hex = NULL;
	g_autofree guint8 *salt = random_bytes(32, 0xd6368505);
	g_autofree gchar *salt_hex = r_hex_encode(salt, 32);
	off_t combined_size;
	int dmfd = -1;

	/* needs to run as root */
	if (!test_running_as_root())
		return;

	filename = g_build_filename(fixture->tmpdir, "data", NULL);
	g_assert_nonnull(filename);
	content = random_bytes(4096*dm_data->data_size, 0x0fdfc761);
	g_assert_true(g_file_set_contents(filename, (gchar *)content, 4096*dm_data->data_size, NULL));

	bundlefd = g_open(filename, O_RDWR);
	g_assert_cmpint(bundlefd, >, 0);

	/* create verity file */
	ret = verity_create_or_verify_hash(0, bundlefd, dm_data->data_size, &combined_size, root_hash, salt);
	g_assert_cmpint(ret, ==, 0);
	g_assert_cmpint(combined_size, ==, dm_data->combined_size);
	root_hash_hex = r_hex_encode(root_hash, 32);

	/* check unmodified verity file */
	ret = verity_create_or_verify_hash(1, bundlefd, dm_data->data_size, NULL, root_hash, salt);
	g_assert_cmpint(ret, ==, 0);

	/* open via kernel loopback device and dm-verity */
	dmfd = open_loop_verity(bundlefd, 4096*dm_data->combined_size, 4096*dm_data->data_size, root_hash_hex, salt_hex, &error);
	g_assert_no_error(error);
	g_assert_cmpint(dmfd, >=, 0);

	/* check that everything is readable */
	drop_caches();
	g_assert_cmpint(readable_sectors(dmfd, content, 4096*dm_data->data_size), ==, dm_data->data_size);

	g_test_message("checking error detection in the first sector");
	/* flip one bit in the first sector */
	flip_bits_filename(filename, 0, 0x01);

	/* check that the bit flip in the first sector is detected by the userspace check */
	ret = verity_create_or_verify_hash(1, bundlefd, dm_data->data_size, NULL, root_hash, salt);
	g_assert_cmpint(ret, !=, 0);

	/* check that only the affected sector is unreadable */
	drop_caches();
	g_assert_cmpint(readable_sectors(dmfd, content, 4096*dm_data->data_size), ==, dm_data->data_size - 1);

	g_close(dmfd, NULL);

	/* retry opening the modified verity file */
	dmfd = open_loop_verity(bundlefd, 4096*dm_data->combined_size, 4096*dm_data->data_size, root_hash_hex, salt_hex, &error);
	g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED);
	g_assert_cmpstr(error->message, ==, "Check read from dm-verity device failed: Input/output error");
	g_assert_cmpint(dmfd, ==, -1);
	g_clear_error(&error);

	/* restore the first sector */
	flip_bits_filename(filename, 0, 0x01);

	if (dm_data->data_size >= 128) {
		g_test_message("checking error detection in another sector");

		/* flip one bit */
		flip_bits_filename(filename, 4096*127, 0x01);

		/* open via kernel loopback device and dm-verity */
		dmfd = open_loop_verity(bundlefd, 4096*dm_data->combined_size, 4096*dm_data->data_size, root_hash_hex, salt_hex, &error);
		g_assert_no_error(error);
		g_assert_cmpint(dmfd, >=, 0);

		/* check that only the affected sector is unreadable */
		drop_caches();
		g_assert_cmpint(readable_sectors(dmfd, content, 4096*dm_data->data_size), ==, dm_data->data_size - 1);

		/* check that the bit flip is detected by the userspace check */
		ret = verity_create_or_verify_hash(1, bundlefd, dm_data->data_size, NULL, root_hash, salt);
		g_assert_cmpint(ret, !=, 0);

		g_close(dmfd, NULL);
	}

	g_close(bundlefd, NULL);
}

int main(int argc, char *argv[])
{
	DMData *dm_data;

	setlocale(LC_ALL, "C");

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/dm/verity_simple", dm_verity_simple_test);
	g_test_add_func("/dm/verity_hash", verity_hash_test);

	dm_data = &(DMData) {
		.data_size = 1,
		.combined_size = 1,
	};
	g_test_add("/dm/create_1", DMFixture, dm_data, dm_fixture_set_up, verity_hash_create, dm_fixture_tear_down);

	dm_data = &(DMData) {
		.data_size = 2,
		.combined_size = 2+1,
	};
	g_test_add("/dm/create_2", DMFixture, dm_data, dm_fixture_set_up, verity_hash_create, dm_fixture_tear_down);

	dm_data = &(DMData) {
		.data_size = 128,
		.combined_size = 128+1,
	};
	g_test_add("/dm/create_128", DMFixture, dm_data, dm_fixture_set_up, verity_hash_create, dm_fixture_tear_down);

	dm_data = &(DMData) {
		.data_size = 257,
		.combined_size = 257+3+1,
	};
	g_test_add("/dm/create_257", DMFixture, dm_data, dm_fixture_set_up, verity_hash_create, dm_fixture_tear_down);

	return g_test_run();
}
