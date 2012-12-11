// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "gtest/gtest.h"
#include "include/cephfs/libcephfs.h"
#include <errno.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/xattr.h>
#include <sstream>
#include <string>
#include <algorithm>
#include "json_spirit/json_spirit.h"
#include "tools/tools.h"

/*
 * The bool parameter to control localized reads isn't used in
 * ConfiguredMountTest, and is only really needed in MountedTest. This is
 * possible with gtest 1.6, but not 1.5 Maybe time to upgrade.
 */
class ConfiguredMountTest : public ::testing::TestWithParam<bool> {
  protected:
    struct ceph_mount_info *cmount;

    virtual void SetUp() {
      ASSERT_EQ(ceph_create(&cmount, NULL), 0);
      ASSERT_EQ(ceph_conf_read_file(cmount, NULL), 0);
    }

    virtual void TearDown() {
      ASSERT_EQ(ceph_release(cmount), 0);
    }

    void RefreshMount() {
      ASSERT_EQ(ceph_release(cmount), 0);
      ASSERT_EQ(ceph_create(&cmount, NULL), 0);
      ASSERT_EQ(ceph_conf_read_file(cmount, NULL), 0);
    }
};

class MountedTest : public ConfiguredMountTest {

  std::string root, asok;

  protected:
    virtual void SetUp() {
      /* Grab test names to build clean room directory name */
      const ::testing::TestInfo* const test_info =
        ::testing::UnitTest::GetInstance()->current_test_info();

      /* Create unique string using test/testname/pid */
      std::stringstream ss_unique;
      ss_unique << test_info->test_case_name() << "_" << test_info->name() << "_" << getpid();
      std::string unique_path = ss_unique.str();
      std::replace(unique_path.begin(), unique_path.end(), '/', '_');

      /* Make absolute directory for mount root point */
      root = unique_path;
      root.insert(0, 1, '/');

      /* Make /tmp path for client admin socket */
      asok = unique_path;
      asok.insert(0, "/tmp/");

      /* Now mount */
      ConfiguredMountTest::SetUp();
      Mount();
    }

    virtual void TearDown() {
      ASSERT_EQ(ceph_unmount(cmount), 0);
      ConfiguredMountTest::TearDown();
    }

    void Remount(bool deep=false) {
      ASSERT_EQ(ceph_unmount(cmount), 0);
      if (deep)
        ConfiguredMountTest::RefreshMount();
      Mount();
    }

    uint64_t get_objecter_replica_ops() {
      /* Grab and parse the perf data if we haven't already */
      std::stringstream ss;
      ceph_tool_do_admin_socket(asok, "perf dump", ss);
      std::string perfdump = ss.str();

      json_spirit::mValue perfval;
      json_spirit::read(perfdump, perfval);

      json_spirit::mValue objecterVal = perfval.get_obj().find("objecter")->second;
      json_spirit::mValue replicaVal = objecterVal.get_obj().find("op_send_replica")->second;

      return replicaVal.get_uint64();
    }

  private:
    void Mount() {
      /* Setup clean room root directory */
      ASSERT_EQ(ceph_mount(cmount, "/"), 0);

      struct stat st;
      int ret = ceph_stat(cmount, root.c_str(), &st);
      if (ret == -ENOENT)
        ASSERT_EQ(ceph_mkdir(cmount, root.c_str(), 0700), 0);
      else {
        ASSERT_EQ(ret, 0);
        ASSERT_TRUE(S_ISDIR(st.st_mode));
      }

      /* Create completely fresh mount context */
      ASSERT_EQ(ceph_unmount(cmount), 0);
      ConfiguredMountTest::RefreshMount();

      /* Setup admin socket */
      ASSERT_EQ(ceph_conf_set(cmount, "admin_socket", asok.c_str()), 0);

      /* Mount with new root directory */
      ASSERT_EQ(ceph_mount(cmount, root.c_str()), 0);

      /* Use localized reads for this mount? */
      bool localize = GetParam();
      ASSERT_EQ(ceph_localize_reads(cmount, localize), 0);
    }
};

TEST_P(MountedTest, OpenEmptyComponent) {

  pid_t mypid = getpid();

  char c_dir[1024];
  sprintf(c_dir, "/open_test_%d", mypid);
  struct ceph_dir_result *dirp;

  ASSERT_EQ(0, ceph_mkdirs(cmount, c_dir, 0777));

  ASSERT_EQ(0, ceph_opendir(cmount, c_dir, &dirp));

  char c_path[1024];
  sprintf(c_path, "/open_test_%d//created_file_%d", mypid, mypid);
  int fd = ceph_open(cmount, c_path, O_RDONLY|O_CREAT, 0666);
  ASSERT_LT(0, fd);

  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_closedir(cmount, dirp));

  Remount();

  fd = ceph_open(cmount, c_path, O_RDONLY, 0666);
  ASSERT_LT(0, fd);

  ASSERT_EQ(0, ceph_close(cmount, fd));
}

TEST_P(ConfiguredMountTest, MountNonExist) {
  ASSERT_NE(0, ceph_mount(cmount, "/non-exist"));
}

TEST_P(MountedTest, MountDouble) {
  ASSERT_EQ(-EISCONN, ceph_mount(cmount, "/"));
}

TEST_P(MountedTest, MountRemount) {
  CephContext *cct = ceph_get_mount_context(cmount);
  Remount();
  ASSERT_EQ(cct, ceph_get_mount_context(cmount));
}

TEST_P(ConfiguredMountTest, UnmountUnmounted) {
  ASSERT_EQ(-ENOTCONN, ceph_unmount(cmount));
}

TEST_P(ConfiguredMountTest, ReleaseUnmounted) {
  // Default behavior of ConfiguredMountTest
}

TEST_P(MountedTest, ReleaseMounted) {
  ASSERT_EQ(-EISCONN, ceph_release(cmount));
}

TEST_P(MountedTest, UnmountRelease) {
  // Default behavior of ConfiguredMountTest
}

TEST_P(MountedTest, Mount) {
  /*
   * Remount(true) will reproduce the following. The first mount operation is
   * taken care of by the fixture.
   *
   * struct ceph_mount_info *cmount;
   * ASSERT_EQ(ceph_create(&cmount, NULL), 0);
   * ASSERT_EQ(ceph_conf_read_file(cmount, NULL), 0);
   * ASSERT_EQ(ceph_mount(cmount, NULL), 0);
   * ceph_shutdown(cmount);
   *
   * ASSERT_EQ(ceph_create(&cmount, NULL), 0);
   * ASSERT_EQ(ceph_conf_read_file(cmount, NULL), 0);
   * ASSERT_EQ(ceph_mount(cmount, NULL), 0);
   * ceph_shutdown(cmount);
   */
  Remount(true);
}

TEST_P(MountedTest, OpenLayout) {
  /* valid layout */
  char test_layout_file[256];
  sprintf(test_layout_file, "test_layout_%d_b", getpid());
  int fd = ceph_open_layout(cmount, test_layout_file, O_CREAT, 0666, (1<<20), 7, (1<<20), NULL);
  ASSERT_GT(fd, 0);
  ceph_close(cmount, fd);

  /* invalid layout */
  sprintf(test_layout_file, "test_layout_%d_c", getpid());
  fd = ceph_open_layout(cmount, test_layout_file, O_CREAT, 0666, (1<<20), 1, 19, NULL);
  ASSERT_EQ(fd, -EINVAL);
  ceph_close(cmount, fd);
}

TEST_P(MountedTest, DirLs) {

  pid_t mypid = getpid();

  struct ceph_dir_result *ls_dir = NULL;
  char foostr[256];
  sprintf(foostr, "dir_ls%d", mypid);
  ASSERT_EQ(ceph_opendir(cmount, foostr, &ls_dir), -ENOENT);

  ASSERT_EQ(ceph_mkdir(cmount, foostr, 0777), 0);
  struct stat stbuf;
  ASSERT_EQ(ceph_stat(cmount, foostr, &stbuf), 0);
  ASSERT_NE(S_ISDIR(stbuf.st_mode), 0);

  char barstr[256];
  sprintf(barstr, "dir_ls2%d", mypid);
  ASSERT_EQ(ceph_lstat(cmount, barstr, &stbuf), -ENOENT);

  // insert files into directory and test open
  char bazstr[256];
  int i = 0, r = rand() % 4096;
  if (getenv("LIBCEPHFS_RAND")) {
    r = atoi(getenv("LIBCEPHFS_RAND"));
  }
  printf("rand: %d\n", r);
  for(; i < r; ++i) {

    sprintf(bazstr, "dir_ls%d/dirf%d", mypid, i);
    int fd  = ceph_open(cmount, bazstr, O_CREAT|O_RDONLY, 0666);
    ASSERT_GT(fd, 0);
    ASSERT_EQ(ceph_close(cmount, fd), 0);

    // set file sizes for readdirplus
    ceph_truncate(cmount, bazstr, i);
  }

  ASSERT_EQ(ceph_opendir(cmount, foostr, &ls_dir), 0);

  // not guaranteed to get . and .. first, but its a safe assumption in this case
  struct dirent *result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, ".");
  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, "..");

  std::vector<std::pair<char *, int> > entries;
  // check readdir and capture stream order for future tests
  for(i = 0; i < r; ++i) {

    result = ceph_readdir(cmount, ls_dir);
    ASSERT_TRUE(result != NULL);

    int size;
    sscanf(result->d_name, "dirf%d", &size);
    entries.push_back(std::pair<char*,int>(strdup(result->d_name), size));
  }

  ASSERT_TRUE(ceph_readdir(cmount, ls_dir) == NULL);

  // test rewinddir
  ceph_rewinddir(cmount, ls_dir);

  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, ".");
  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, "..");

  // check telldir
  for(i = 0; i < r-1; ++i) {
    int r = ceph_telldir(cmount, ls_dir);
    ASSERT_GT(r, -1);
    ceph_seekdir(cmount, ls_dir, r);
    result = ceph_readdir(cmount, ls_dir);
    ASSERT_TRUE(result != NULL);
    ASSERT_STREQ(result->d_name, entries[i].first);
  }

  ceph_rewinddir(cmount, ls_dir);

  int t = ceph_telldir(cmount, ls_dir);
  ASSERT_GT(t, -1);

  ASSERT_TRUE(ceph_readdir(cmount, ls_dir) != NULL);

  // test seekdir - move back to the beginning
  ceph_seekdir(cmount, ls_dir, t);

  // test getdents
  struct dirent *getdents_entries;
  getdents_entries = (struct dirent *)malloc(r * sizeof(*getdents_entries));

  int count = 0;
  while (count < r) {
    int len = ceph_getdents(cmount, ls_dir, (char *)getdents_entries, r * sizeof(*getdents_entries));
    ASSERT_GT(len, 0);
    ASSERT_TRUE((len % sizeof(*getdents_entries)) == 0);
    int n = len / sizeof(*getdents_entries);
    if (count == 0) {
      ASSERT_STREQ(getdents_entries[0].d_name, ".");
      ASSERT_STREQ(getdents_entries[1].d_name, "..");
    }
    int j;
    i = count;
    for(j = 2; j < n; ++i, ++j) {
      ASSERT_STREQ(getdents_entries[j].d_name, entries[i].first);
    }
    count += n;
  }

  free(getdents_entries);

  // test readdir_r
  ceph_rewinddir(cmount, ls_dir);

  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, ".");
  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, "..");

  for(i = 0; i < r; ++i) {
    struct dirent rdent;
    ASSERT_EQ(ceph_readdir_r(cmount, ls_dir, &rdent), 1);
    ASSERT_STREQ(rdent.d_name, entries[i].first);
  }

  // test readdirplus
  ceph_rewinddir(cmount, ls_dir);

  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, ".");
  result = ceph_readdir(cmount, ls_dir);
  ASSERT_TRUE(result != NULL);
  ASSERT_STREQ(result->d_name, "..");

  for(i = 0; i < r; ++i) {
    struct dirent rdent;
    struct stat st;
    int stmask;
    ASSERT_EQ(ceph_readdirplus_r(cmount, ls_dir, &rdent, &st, &stmask), 1);
    ASSERT_STREQ(rdent.d_name, entries[i].first);
    ASSERT_EQ(st.st_size, entries[i].second);
    ASSERT_EQ(st.st_ino, rdent.d_ino);
    //ASSERT_EQ(st.st_mode, (mode_t)0666);
  }

  ASSERT_EQ(ceph_closedir(cmount, ls_dir), 0);
}

TEST_P(MountedTest, ManyNestedDirs) {
  const char *many_path = "a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a";
  ASSERT_EQ(ceph_mkdirs(cmount, many_path, 0755), 0);

  int i = 0;

  for(; i < 39; ++i) {
    ASSERT_EQ(ceph_chdir(cmount, "a"), 0);

    struct ceph_dir_result *dirp;
    ASSERT_EQ(ceph_opendir(cmount, "a", &dirp), 0);
    struct dirent *dent = ceph_readdir(cmount, dirp);
    ASSERT_TRUE(dent != NULL);
    ASSERT_STREQ(dent->d_name, ".");
    dent = ceph_readdir(cmount, dirp);
    ASSERT_TRUE(dent != NULL);
    ASSERT_STREQ(dent->d_name, "..");
    dent = ceph_readdir(cmount, dirp);
    ASSERT_TRUE(dent != NULL);
    ASSERT_STREQ(dent->d_name, "a");
    ASSERT_EQ(ceph_closedir(cmount, dirp), 0);
  }

  ASSERT_STREQ(ceph_getcwd(cmount), "/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a");

  ASSERT_EQ(ceph_chdir(cmount, "a/a/a"), 0);

  for(i = 0; i < 39; ++i) {
    ASSERT_EQ(ceph_chdir(cmount, ".."), 0);
    ASSERT_EQ(ceph_rmdir(cmount, "a"), 0);
  }

  ASSERT_EQ(ceph_chdir(cmount, "/"), 0);

  ASSERT_EQ(ceph_rmdir(cmount, "a/a/a"), 0);
}

TEST_P(MountedTest, Xattrs) {
  char test_xattr_file[256];
  sprintf(test_xattr_file, "test_xattr_%d", getpid());
  int fd = ceph_open(cmount, test_xattr_file, O_CREAT, 0666);
  ASSERT_GT(fd, 0);

  char i = 'a';
  char xattrk[128];
  char xattrv[128];
  for(; i < 'a'+26; ++i) {
    sprintf(xattrk, "user.test_xattr_%c", i);
    int len = sprintf(xattrv, "testxattr%c", i);
    ASSERT_EQ(ceph_setxattr(cmount, test_xattr_file, xattrk, (void *) xattrv, len, XATTR_CREATE), 0);
  }

  char xattrlist[128*26];
  int len = ceph_listxattr(cmount, test_xattr_file, xattrlist, sizeof(xattrlist));
  char *p = xattrlist;
  char *n;
  i = 'a';
  while(len > 0) {
    sprintf(xattrk, "user.test_xattr_%c", i);
    ASSERT_STREQ(p, xattrk);

    char gxattrv[128];
    int alen = ceph_getxattr(cmount, test_xattr_file, p, (void *) gxattrv, 128);
    sprintf(xattrv, "testxattr%c", i);
    ASSERT_TRUE(!strncmp(xattrv, gxattrv, alen));

    n = index(p, '\0');
    n++;
    len -= (n - p);
    p = n;
    ++i;
  }

  i = 'a';
  for(i = 'a'; i < 'a'+26; ++i) {
    sprintf(xattrk, "user.test_xattr_%c", i);
    ASSERT_EQ(ceph_removexattr(cmount, test_xattr_file, xattrk), 0);
  }

  ceph_close(cmount, fd);
}

TEST_P(MountedTest, LstatSlashdot) {
  struct stat stbuf;
  ASSERT_EQ(ceph_lstat(cmount, "/.", &stbuf), 0);
  ASSERT_EQ(ceph_lstat(cmount, ".", &stbuf), 0);
}

TEST_P(MountedTest, DoubleChmod) {

  char test_file[256];
  sprintf(test_file, "test_perms_%d", getpid());

  int fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0666);
  ASSERT_GT(fd, 0);

  // write some stuff
  const char *bytes = "foobarbaz";
  ASSERT_EQ(ceph_write(cmount, fd, bytes, strlen(bytes), 0), (int)strlen(bytes));

  ceph_close(cmount, fd);

  // set perms to read but can't write
  ASSERT_EQ(ceph_chmod(cmount, test_file, 0400), 0);

  fd = ceph_open(cmount, test_file, O_RDWR, 0);
  ASSERT_EQ(fd, -EACCES);

  fd = ceph_open(cmount, test_file, O_RDONLY, 0);
  ASSERT_GT(fd, -1);

  char buf[100];
  int ret = ceph_read(cmount, fd, buf, 100, 0);
  ASSERT_EQ(ret, (int)strlen(bytes));
  buf[ret] = '\0';
  ASSERT_STREQ(buf, bytes);

  ASSERT_EQ(ceph_write(cmount, fd, bytes, strlen(bytes), 0), -EBADF);

  ceph_close(cmount, fd);

  // reset back to writeable
  ASSERT_EQ(ceph_chmod(cmount, test_file, 0600), 0);

  // ensure perms are correct
  struct stat stbuf;
  ASSERT_EQ(ceph_lstat(cmount, test_file, &stbuf), 0);
  ASSERT_EQ(stbuf.st_mode, 0100600U);

  fd = ceph_open(cmount, test_file, O_RDWR, 0);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(ceph_write(cmount, fd, bytes, strlen(bytes), 0), (int)strlen(bytes));
  ceph_close(cmount, fd);
}

TEST_P(MountedTest, Fchmod) {
  char test_file[256];
  sprintf(test_file, "test_perms_%d", getpid());

  int fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0666);
  ASSERT_GT(fd, 0);

  // write some stuff
  const char *bytes = "foobarbaz";
  ASSERT_EQ(ceph_write(cmount, fd, bytes, strlen(bytes), 0), (int)strlen(bytes));

  // set perms to read but can't write
  ASSERT_EQ(ceph_fchmod(cmount, fd, 0400), 0);

  char buf[100];
  int ret = ceph_read(cmount, fd, buf, 100, 0);
  ASSERT_EQ(ret, (int)strlen(bytes));
  buf[ret] = '\0';
  ASSERT_STREQ(buf, bytes);

  ASSERT_EQ(ceph_write(cmount, fd, bytes, strlen(bytes), 0), (int)strlen(bytes));

  ceph_close(cmount, fd);

  ASSERT_EQ(ceph_open(cmount, test_file, O_RDWR, 0), -EACCES);

  // reset back to writeable
  ASSERT_EQ(ceph_chmod(cmount, test_file, 0600), 0);

  fd = ceph_open(cmount, test_file, O_RDWR, 0);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(ceph_write(cmount, fd, bytes, strlen(bytes), 0), (int)strlen(bytes));
  ceph_close(cmount, fd);
}

TEST_P(MountedTest, Fchown) {
  char test_file[256];
  sprintf(test_file, "test_fchown_%d", getpid());

  int fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0666);
  ASSERT_GT(fd, 0);

  // set perms to readable and writeable only by owner
  ASSERT_EQ(ceph_fchmod(cmount, fd, 0600), 0);

  // change ownership to nobody -- we assume nobody exists and id is always 65534
  ASSERT_EQ(ceph_fchown(cmount, fd, 65534, 65534), 0);

  ceph_close(cmount, fd);

  fd = ceph_open(cmount, test_file, O_RDWR, 0);
  ASSERT_EQ(fd, -EACCES);
}

TEST_P(MountedTest, Symlinks) {
  char test_file[256];
  sprintf(test_file, "test_symlinks_%d", getpid());

  int fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0666);
  ASSERT_GT(fd, 0);

  ceph_close(cmount, fd);

  char test_symlink[256];
  sprintf(test_symlink, "test_symlinks_sym_%d", getpid());

  ASSERT_EQ(ceph_symlink(cmount, test_file, test_symlink), 0);

  // stat the original file
  struct stat stbuf_orig;
  ASSERT_EQ(ceph_stat(cmount, test_file, &stbuf_orig), 0);
  // stat the symlink
  struct stat stbuf_symlink_orig;
  ASSERT_EQ(ceph_stat(cmount, test_symlink, &stbuf_symlink_orig), 0);
  // ensure the stat bufs are equal
  ASSERT_TRUE(!memcmp(&stbuf_orig, &stbuf_symlink_orig, sizeof(stbuf_orig)));

  sprintf(test_file, "/test_symlinks_abs_%d", getpid());

  fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0666);
  ASSERT_GT(fd, 0);

  ceph_close(cmount, fd);

  sprintf(test_symlink, "/test_symlinks_abs_sym_%d", getpid());

  ASSERT_EQ(ceph_symlink(cmount, test_file, test_symlink), 0);

  // stat the original file
  ASSERT_EQ(ceph_stat(cmount, test_file, &stbuf_orig), 0);
  // stat the symlink
  ASSERT_EQ(ceph_stat(cmount, test_symlink, &stbuf_symlink_orig), 0);
  // ensure the stat bufs are equal
  ASSERT_TRUE(!memcmp(&stbuf_orig, &stbuf_symlink_orig, sizeof(stbuf_orig)));

  // test lstat
  struct stat stbuf_symlink;
  ASSERT_EQ(ceph_lstat(cmount, test_symlink, &stbuf_symlink), 0);
  ASSERT_TRUE(S_ISLNK(stbuf_symlink.st_mode));
}

TEST_P(MountedTest, DirSyms) {
  char test_dir1[256];
  sprintf(test_dir1, "dir1_symlinks_%d", getpid());

  ASSERT_EQ(ceph_mkdir(cmount, test_dir1, 0700), 0);

  char test_symdir[256];
  sprintf(test_symdir, "symdir_symlinks_%d", getpid());

  ASSERT_EQ(ceph_symlink(cmount, test_dir1, test_symdir), 0);

  char test_file[256];
  sprintf(test_file, "/symdir_symlinks_%d/test_symdir_file", getpid());
  int fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0600);
  ASSERT_GT(fd, 0);
  ceph_close(cmount, fd);

  struct stat stbuf;
  ASSERT_EQ(ceph_lstat(cmount, test_file, &stbuf), 0);

  // ensure that its a file not a directory we get back
  ASSERT_TRUE(S_ISREG(stbuf.st_mode));
}

TEST_P(MountedTest, LoopSyms) {
  char test_dir1[256];
  sprintf(test_dir1, "dir1_loopsym_%d", getpid());

  ASSERT_EQ(ceph_mkdir(cmount, test_dir1, 0700), 0);

  char test_dir2[256];
  sprintf(test_dir2, "/dir1_loopsym_%d/loop_dir", getpid());

  ASSERT_EQ(ceph_mkdir(cmount, test_dir2, 0700), 0);

  // symlink it itself:  /path/to/mysym -> /path/to/mysym
  char test_symdir[256];
  sprintf(test_symdir, "/dir1_loopsym_%d/loop_dir/symdir", getpid());

  ASSERT_EQ(ceph_symlink(cmount, test_symdir, test_symdir), 0);

  char test_file[256];
  sprintf(test_file, "/dir1_loopsym_%d/loop_dir/symdir/test_loopsym_file", getpid());
  int fd = ceph_open(cmount, test_file, O_CREAT|O_RDWR, 0600);
  ASSERT_EQ(fd, -ELOOP);

  // loop: /a -> /b, /b -> /c, /c -> /a
  char a[256], b[256], c[256];
  sprintf(a, "/%s/a", test_dir1);
  sprintf(b, "/%s/b", test_dir1);
  sprintf(c, "/%s/c", test_dir1);
  ASSERT_EQ(ceph_symlink(cmount, a, b), 0);
  ASSERT_EQ(ceph_symlink(cmount, b, c), 0);
  ASSERT_EQ(ceph_symlink(cmount, c, a), 0);
  ASSERT_EQ(ceph_open(cmount, a, O_RDWR, 0), -ELOOP);
}

TEST_P(MountedTest, HardlinkNoOriginal) {

  int mypid = getpid();

  char dir[256];
  sprintf(dir, "/test_rmdirfail%d", mypid);
  ASSERT_EQ(ceph_mkdir(cmount, dir, 0777), 0);

  ASSERT_EQ(ceph_chdir(cmount, dir), 0);

  int fd = ceph_open(cmount, "f1", O_CREAT, 0644);
  ASSERT_GT(fd, 0);

  ceph_close(cmount, fd);

  // create hard link
  ASSERT_EQ(ceph_link(cmount, "f1", "hardl1"), 0);

  // remove file link points to
  ASSERT_EQ(ceph_unlink(cmount, "f1"), 0);

  /* Complete refresh (builds new context) */
  Remount(true);

  // now cleanup
  ASSERT_EQ(ceph_chdir(cmount, dir), 0);
  ASSERT_EQ(ceph_unlink(cmount, "hardl1"), 0);
  ASSERT_EQ(ceph_rmdir(cmount, dir), 0);
}

TEST_P(MountedTest, BadFileDesc) {
  ASSERT_EQ(ceph_fchmod(cmount, -1, 0655), -EBADF);
  ASSERT_EQ(ceph_close(cmount, -1), -EBADF);
  ASSERT_EQ(ceph_lseek(cmount, -1, 0, SEEK_SET), -EBADF);

  char buf[0];
  ASSERT_EQ(ceph_read(cmount, -1, buf, 0, 0), -EBADF);
  ASSERT_EQ(ceph_write(cmount, -1, buf, 0, 0), -EBADF);

  ASSERT_EQ(ceph_ftruncate(cmount, -1, 0), -EBADF);
  ASSERT_EQ(ceph_fsync(cmount, -1, 0), -EBADF);

  struct stat stat;
  ASSERT_EQ(ceph_fstat(cmount, -1, &stat), -EBADF);

  struct sockaddr_storage addr;
  ASSERT_EQ(ceph_get_file_stripe_address(cmount, -1, 0, &addr, 1), -EBADF);

  ASSERT_EQ(ceph_get_file_stripe_unit(cmount, -1), -EBADF);
  ASSERT_EQ(ceph_get_file_pool(cmount, -1), -EBADF);
  ASSERT_EQ(ceph_get_file_replication(cmount, -1), -EBADF);
}

TEST_P(MountedTest, ReadEmptyFile) {
  // test the read_sync path in the client for zero files
  ASSERT_EQ(ceph_conf_set(cmount, "client_debug_force_sync_read", "true"), 0);

  int mypid = getpid();
  char testf[256];

  sprintf(testf, "test_reademptyfile%d", mypid);
  int fd = ceph_open(cmount, testf, O_CREAT|O_TRUNC|O_WRONLY, 0644);
  ASSERT_GT(fd, 0);

  ceph_close(cmount, fd);

  fd = ceph_open(cmount, testf, O_RDONLY, 0);
  ASSERT_GT(fd, 0);

  char buf[4096];
  ASSERT_EQ(ceph_read(cmount, fd, buf, 4096, 0), 0);

  ceph_close(cmount, fd);
}

TEST_P(MountedTest, ReaddirRCB) {
  char c_dir[256];
  sprintf(c_dir, "/readdir_r_cb_tests_%d", getpid());
  struct ceph_dir_result *dirp;
  ASSERT_EQ(0, ceph_mkdirs(cmount, c_dir, 0777));
  ASSERT_LE(0, ceph_opendir(cmount, c_dir, &dirp));

  // dir is empty, check that it only contains . and ..
  int buflen = 100;
  char *buf = new char[buflen];
  // . is 2, .. is 3 (for null terminators)
  ASSERT_EQ(5, ceph_getdnames(cmount, dirp, buf, buflen));
  char c_file[256];
  sprintf(c_file, "/readdir_r_cb_tests_%d/foo", getpid());
  int fd = ceph_open(cmount, c_file, O_CREAT, 0777);
  ASSERT_LT(0, fd);

  // check correctness with one entry
  ASSERT_LE(0, ceph_closedir(cmount, dirp));
  ASSERT_LE(0, ceph_opendir(cmount, c_dir, &dirp));
  ASSERT_EQ(9, ceph_getdnames(cmount, dirp, buf, buflen)); // ., .., foo

  // check correctness if buffer is too small
  ASSERT_LE(0, ceph_closedir(cmount, dirp));
  ASSERT_GE(0, ceph_opendir(cmount, c_dir, &dirp));
  ASSERT_EQ(-ERANGE, ceph_getdnames(cmount, dirp, buf, 1));

  //check correctness if it needs to split listing
  ASSERT_LE(0, ceph_closedir(cmount, dirp));
  ASSERT_LE(0, ceph_opendir(cmount, c_dir, &dirp));
  ASSERT_EQ(5, ceph_getdnames(cmount, dirp, buf, 6));
  ASSERT_EQ(4, ceph_getdnames(cmount, dirp, buf, 6));
  ASSERT_LE(0, ceph_closedir(cmount, dirp));
}

INSTANTIATE_TEST_CASE_P(ParamMount, MountedTest,
    ::testing::Values(false, true));

/* false parameter ignored. fix this when gtest upgraded to 1.6 */
INSTANTIATE_TEST_CASE_P(ParamConfiguredMount, ConfiguredMountTest,
    ::testing::Values(false));
