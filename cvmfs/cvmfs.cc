/**
 * \file cvmfs.cc
 * \namespace cvmfs
 *
 * CernVM-FS is a FUSE module which implements an HTTP read-only filesystem.
 * The original idea is based on GROW-FS.
 *
 * CernVM-FS shows a remote HTTP directory as local file system.  The client
 * sees all available files.  On first access, a file is downloaded and
 * cached locally.  All downloaded pieces are verified with SHA1.
 *
 * To do so, a directory hive has to be transformed into a CVMFS2
 * "repository".  This can be done by the CernVM-FS server tools.
 *
 * This preparation of directories is transparent to web servers and
 * web proxies.  They just serve static content, i.e. arbitrary files.
 * Any HTTP server should do the job.  We use Apache + Squid.  Serving
 * files from the memory of a web proxy brings a significant performance
 * improvement.
 */

// TODO: ndownload into cache, Lock on boot / finalize

#define ENOATTR ENODATA /* instead of including attr/xattr.h */

#include "cvmfs_config.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef __APPLE__
#include <sys/statfs.h>
#endif
#include <sys/wait.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/xattr.h>

#include <openssl/crypto.h>

#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <cassert>
#include <cstdio>

#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>

#include "platform.h"
#include "fuse-duplex.h"
#include "logging.h"
#include "tracer.h"
#include "download.h"
#include "cache.h"
#include "hash.h"
#include "talk.h"
#include "monitor.h"
#include "signature.h"
#include "lru.h"
#include "util.h"
#include "atomic.h"
#include "fuse_op_stubs.h"
#include "inode_cache.h"
#include "path_cache.h"
#include "md5path_cache.h"
#include "RemoteCatalogManager.h"
#include "DirectoryEntry.h"
#include "compression.h"
#include "sqlite3-duplex.h"
#include "smalloc.h"

using namespace std;  // NOLINT

namespace cvmfs {
  string mountpoint = "";
  string root_url = "";
  string root_catalog = "";
  string cachedir = "/var/cache/cvmfs2/default";
  string proxies = "";
  string whitelist = "";
  /** blacklist for compromised certificates */
  string blacklist = "/etc/cvmfs/blacklist";
  string deep_mount = "";
  string repo_name = "";  /**< Expected repository name, e.g. atlas.cern.ch */
  const double whitelist_lifetime = 3600.0*24.0*30.0; /**< 30 days in seconds */
  const int short_term_ttl = 240; /* in offline mode, check every 4 minutes */
  string pubkey = "/etc/cvmfs/keys/cern.ch.pub";
  string tracefile = "";
  uid_t uid = 0;                /**< will be set to uid of launching user. */
  gid_t gid = 0;                /**< will be set to gid of launching user. */
  pid_t pid = 0;                /**< will be set after deamon() */
  bool force_signing = false;   /**< Do not load not-signed catalogs. */
  unsigned max_ttl = 0;
  pthread_mutex_t mutex_max_ttl = PTHREAD_MUTEX_INITIALIZER;
  int max_cache_timeout = 0;
  time_t drainout_deadline = 0;
  hash::Any next_root(hash::kSha1);
  time_t boot_time;
  struct fuse_lowlevel_ops fuseCallbacks;

  RemoteCatalogManager *catalog_manager;

  const unsigned int inode_cache_size = 20000;
  InodeCache *inode_cache = NULL;
  const unsigned int path_cache_size = 1000;
  PathCache *path_cache = NULL;
  const unsigned int md5path_cache_size = 1000;
  Md5PathCache *md5path_cache = NULL;

  /* Prevent DoS attacks on the Squid server */
  static struct {
    time_t timestamp;
    int delay;
  } prev_io_error;
  /** Maximum start value for exponential backoff */
  const int MAX_INIT_IO_DELAY = 32;
  const int MAX_IO_DELAY = 2000; /**< Maximum 2 seconds */
  const int FORGET_DOS = 10000; /**< Clear DoS memory after 10 seconds */

  // Caches
  atomic_int32 cache_inserts;
  atomic_int32 cache_replaces;
  atomic_int32 cache_cleans;
  atomic_int32 cache_hits;
  atomic_int32 cache_misses;

  atomic_int32 certificate_hits;
  atomic_int32 certificate_misses;

  atomic_int64 nopen;
  atomic_int64 ndownload;

  atomic_int32 open_files; /**< number of currently open files by Fuse calls */
  unsigned nofiles; /**< maximum allowed number of open files */
  /** number of reserved file descriptors for internal use */
  const int NUM_RESERVED_FD = 512;
  atomic_int32 nioerr;

  unsigned get_max_ttl() {
    pthread_mutex_lock(&mutex_max_ttl);
    const unsigned current_max = max_ttl/60;
    pthread_mutex_unlock(&mutex_max_ttl);

    return current_max;
  }

  void set_max_ttl(const unsigned value) {
    pthread_mutex_lock(&mutex_max_ttl);
    max_ttl = value*60;
    pthread_mutex_unlock(&mutex_max_ttl);
  }

  /**
   * Carefully invalidate memory cache
   */
  static void invalidate_cache(const int catalog_id) {
    if (path_cache != NULL && inode_cache != NULL && catalog_id >= 0) {
      LogCvmfs(kLogCvmfs, kLogDebug, "dropping caches for catalog: %d",
               catalog_id);

      // int minInode = catalog::get_inode_offset_for_catalog_id(catalog_id)+1;
      // int maxInode = minInode + catalog::get_num_dirent(catalog_id);
      //
      // for (int i = minInode; i < maxInode; ++i) {
      //    path_cache->forget(i);
      //    inode_cache->forget(i);
      // }

      path_cache->drop();
      inode_cache->drop();
      md5path_cache->drop();
    }
  }


  /**
   * Don't call without catalog::lock()
   */
  int catalog_cache_memusage_bytes() {
    int result = 0;
    /*for (int i = 0; i < CATALOG_CACHE_SIZE; ++i) {
      result += sizeof(catalog_cacheline);
      result += catalog_cache[i].d.name.capacity();
      result += catalog_cache[i].d.symlink.capacity();
    }*/
    return result;
  }

  void catalog_cache_memusage_slots(int *positive, int *negative, int *all,
                                    int *inserts, int *replaces, int *cleans,
                                    int *hits, int *misses,
                                    int *cert_hits, int *cert_misses) {
   /* *positive = *negative = 0;
    *all = CATALOG_CACHE_SIZE;
    hash::t_md5 null;
    for (int i = 0; i < CATALOG_CACHE_SIZE; ++i) {
      if (!(catalog_cache[i].md5 == null)) {
        if (catalog_cache[i].d.catalog_id == -1)
          (*negative)++;
        else
          (*positive)++;
      }
    }
    *inserts = atomic_read32(&cache_inserts);
    *replaces = atomic_read32(&cache_replaces);
    *cleans = atomic_read32(&cache_cleans);
    *hits = atomic_read32(&cache_hits);
    *misses = atomic_read32(&cache_misses);

    *cert_hits = atomic_read32(&certificate_hits);
    *cert_misses = atomic_read32(&certificate_misses);*/
  }

  static bool get_dirent_for_inode(const fuse_ino_t ino,
                                   DirectoryEntry *dirent) {
    // check the inode cache for speed up
    if (inode_cache->lookup(ino, dirent)) {
      LogCvmfs(kLogInodeCache, kLogDebug, "HIT %d -> '%s'",
               ino, dirent->name().c_str());
      return true;

    } else {
      LogCvmfs(kLogInodeCache, kLogDebug, "MISS %d --> lookup in catalogs",
               ino);

      // lookup inode in catalog
      if (catalog_manager->Lookup(ino, dirent)) {
        LogCvmfs(kLogInodeCache, kLogDebug, "CATALOG HIT %d -> '%s'",
                 dirent->inode(), dirent->name().c_str());
        inode_cache->insert(ino, *dirent);
        return true;

      } else {
        LogCvmfs(kLogInodeCache, kLogDebug,
                 "no entry --> maybe data corruption?");
        return false;
      }
    }

    // should be unreachable... just to be sure
    return false;
  }

  static bool get_dirent_for_path(const string &path, DirectoryEntry *dirent) {
    /*
     *  this one is pretty nasty!
     *  in a unit test ../../test/unittests/02....cc
     *  the cache showed reasonable performance (1.2 millon transactions in 4 seconds)
     *  but here it SLOWS DOWN the Davinci benchmark about 20 seconds
     *
     *  there must either be something wrong in the code itself or
     *  I must have overseen some cache coherency problem.
     *
     *  I.e. the data coming out of it is somehow corrupt.
     *  But this is very unlikely,
     *  because the tests do not fail, they are just slower.
     */

    // check the md5path_cache first
    // (TODO: this is a quick and dirty prototype currently!!)
    // it actually slows down the stuff... TODO: find out why
    // if (md5path_cache->lookup(md5, dirent)) {
    //          if (dirent.catalog_id == -1) {
    //             pmesg(D_MD5_CACHE, "HIT NEGATIVE %s -> '%s'",
    //                   md5.to_string().c_str(), dirent.name.c_str());
    //             return false;
    //
    //          } else {
    //             pmesg(D_MD5_CACHE, "HIT %s -> '%s'",
    //                   md5.to_string().c_str(), dirent.name.c_str());
    //             return true;
    //          }
    //       } else {
    LogCvmfs(kLogMd5Cache, kLogDebug, "MISS %s --> lookup in catalogs",
             path.c_str());

    if (catalog_manager->Lookup(path, dirent)) {
      LogCvmfs(kLogMd5Cache, kLogDebug, "CATALOG HIT %s -> '%s'",
            path.c_str(), dirent->name().c_str());
      //            md5path_cache->insert(md5, dirent);
      return true;
    } else {
      // struct catalog::t_dirent negative;
      // negative.catalog_id = -1;
      // negative.name = "negative!";
      //            md5path_cache->insert(md5, negative);
      return false;
    }
    // }

    return false;
  }

  static bool get_path_for_inode(const fuse_ino_t ino, string *path) {
    // check the path cache first
    if (path_cache->lookup(ino, path)) {
      LogCvmfs(kLogPathCache, kLogDebug, "HIT %d -> '%s'", ino, path->c_str());
      return true;  // this was easy!
    }

    LogCvmfs(kLogPathCache, kLogDebug, "MISS %d - recursively building path",
             ino);

    // now we need to find out the parent path recursively and
    // rebuild the absolute path
    string parentPath;
    DirectoryEntry dirent;

    // get the dirent information of the searched inode
    if (not get_dirent_for_inode(ino, &dirent)) {
      return false;
    }

    // check if we reached the root node
    if (dirent.inode() == catalog_manager->GetRootInode()) {
      // encountered root... finished
      *path = "";

    } else {
      // retrieve the parent path recursively
      if (not get_path_for_inode(dirent.parent_inode(), &parentPath)) {
        return false;
      }

      *path = parentPath + "/" + dirent.name();
    }

    path_cache->insert(dirent.inode(), *path);
    return true;
  }


  /**
   * Do a lookup to find out the inode number of a file name
   */
  static void cvmfs_lookup(fuse_req_t req, fuse_ino_t parent,
                           const char *name) {
    parent = catalog_manager->MangleInode(parent);
    LogCvmfs(kLogCvmfs, kLogDebug,
             "cvmfs_lookup in parent inode: %d for name: %s", parent, name);

    string parentPath;

    if (not get_path_for_inode(parent, &parentPath)) {
      LogCvmfs(kLogCvmfs, kLogDebug,
               "no path for inode found... data corrupt?");

      fuse_reply_err(req, ENOENT);
      return;
    }

    DirectoryEntry dirent;
    string path = parentPath + "/" + name;
    bool found_entry = catalog_manager->LookupWithoutParent(path, &dirent);
    if (not found_entry) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    dirent.set_parent_inode(parent);

    // maintain caches
    inode_cache->insert(dirent.inode(), dirent);
    path_cache->insert(dirent.inode(), path);

    // reply
    struct fuse_entry_param result;
    memset(&result, 0, sizeof(result));
    result.ino = dirent.inode();
    result.attr = dirent.GetStatStructure();
    result.attr_timeout = 2.0;  // TODO(rene): replace these magic numbers
    result.entry_timeout = 2.0;

    fuse_reply_entry(req, &result);
  }

  /**
   * Gets called as kind of prerequisit to every operation.
   * We do two kinds of magic here: check catalog TTL (and reload, if necessary)
   * and load nested catalogs. Nested catalogs may also be loaded on readdir.
   *
   * Also, we insert things in our d-cache here.  It is not sufficient to do
   * all the inserts here, even though stat will be called before anything else;
   * they might be cached by the kernel.
   */
  static void cvmfs_getattr(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_getattr (stat) for inode: %d", ino);
    bool found;
    DirectoryEntry dirent;

    found = get_dirent_for_inode(ino, &dirent);

    if (not found) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    /* The actual getattr-work */
    struct stat info = dirent.GetStatStructure();

    fuse_reply_attr(req, &info, 1.0);  // TODO(rene): replace magic number
  }

  /**
   * Reads a symlink from the catalog.  Environment variables are expanded.
   */
  static void cvmfs_readlink(fuse_req_t req, fuse_ino_t ino) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_readlink on inode: %d", ino);
    tracer::Trace(tracer::kFuseReadlink, "no path provided", "readlink() call");
    bool found;
    DirectoryEntry dirent;

    found = get_dirent_for_inode(ino, &dirent);

    if (not found) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    if (not dirent.IsLink()) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    fuse_reply_readlink(req, dirent.symlink().c_str());
  }

  /**
   * Open a file from cache.  If necessary, file is downloaded first.
   * Also catalog reload magic can happen, if file download fails.
   *
   * \return Read-only file descriptor in fi->fh
   */
  static void cvmfs_open(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_open on inode: %d", ino);
    tracer::Trace(tracer::kFuseOpen, "no path provided", "open() call");

    int fd = -1;

    DirectoryEntry d;
    string path;
    bool found;

    found = get_dirent_for_inode(ino, &d) && get_path_for_inode(ino, &path);

    if (not found) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    if ((fi->flags & 3) != O_RDONLY) {
      fuse_reply_err(req, EACCES);
      return;
    }

    fd = cache::Fetch(d, path);
    atomic_inc64(&nopen);

    if (fd >= 0) {
      if (atomic_xadd32(&open_files, 1) <
          (static_cast<int>(nofiles))-NUM_RESERVED_FD) {
        LogCvmfs(kLogCvmfs, kLogDebug, "file %s opened (fd %d)",
                 path.c_str(), fd);
        fi->fh = fd;
        fuse_reply_open(req, fi);
        return;
      } else {
        if (close(fd) == 0) atomic_dec32(&open_files);
        LogCvmfs(kLogCvmfs, kLogSyslog, "open file descriptor limit exceeded");
        fuse_reply_err(req, EMFILE);
        return;
      }
    } else {
      LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog,
               "failed to open inode: %d, CAS key %s, error code %d",
               ino, d.checksum().ToString().c_str(), errno);
      if (errno == EMFILE) {
        fuse_reply_err(req, EMFILE);
        return;
      }
    }

    /* Prevent Squid DoS */
    time_t now = time(NULL);
    if (now-prev_io_error.timestamp < FORGET_DOS) {
      usleep(prev_io_error.delay*1000);
      if (prev_io_error.delay < MAX_IO_DELAY)
        prev_io_error.delay *= 2;
    } else {
      /* Init delay */
      prev_io_error.delay = (random() % (MAX_INIT_IO_DELAY-1)) + 2;
    }
    prev_io_error.timestamp = now;

    atomic_inc32(&nioerr);
    fuse_reply_err(req, EIO);
  }

  /**
   * Redirected to pread into cache.
   */
  static void cvmfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                         struct fuse_file_info *fi) {
    LogCvmfs(kLogCvmfs, kLogDebug,
             "cvmfs_read on inode: %d reading %d bytes from offset %d fd %d",
             catalog_manager->MangleInode(ino), size, off, fi->fh);
    tracer::Trace(tracer::kFuseRead, "path", "read() call");

    // get data chunk
    char *data = static_cast<char *>(alloca(size));
    const int64_t fd = fi->fh;
    int result = pread(fd, data, size, off);

    // push it to user
    if (result >= 0) {
      fuse_reply_buf(req, data, result);
      LogCvmfs(kLogCvmfs, kLogDebug, "pushed %d bytes to user", result);
    } else {
      LogCvmfs(kLogCvmfs, kLogDebug, "read err no %d result %d", errno, result);
      fuse_reply_err(req, errno);
    }
  }


  /**
   * File close operation. Redirected into cache.
   */
  static void cvmfs_release(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi) {
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_release on inode: %d",
             catalog_manager->MangleInode(ino));

    const int64_t fd = fi->fh;
    if (close(fd) == 0) atomic_dec32(&open_files);

    fuse_reply_err(req, 0);
  }

  struct dirListingBuffer {
    char *p;
    size_t size;
  };

  std::map<unsigned int, dirListingBuffer> open_dir_listings;
  unsigned int current_dir_listing_handle = 0;
  pthread_mutex_t open_dir_listings_mutex = PTHREAD_MUTEX_INITIALIZER;

#define min(x, y) ((x) < (y) ? (x) : (y))

  static int replyBufferLimited(fuse_req_t req, const char *buf, size_t bufsize,
                                off_t off, size_t maxsize) {
    if (off < static_cast<int>(bufsize)) {
      return fuse_reply_buf(req, buf + off, min(bufsize - off, maxsize));
    } else {
      return fuse_reply_buf(req, NULL, 0);
    }
  }

  static void addToDirListingBuffer(fuse_req_t req, struct dirListingBuffer *b,
                                    const char *name, struct stat *s) {
    size_t oldsize = b->size;
    b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
    /* TODO: move logic to smalloc */
    char *newp = static_cast<char *>(realloc(b->p, b->size));
    if (!newp) {
      fprintf(stderr, "*** fatal error: cannot allocate memory\n");
      abort();
    }
    b->p = newp;
    fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, s, b->size);
  }

  /**
   * Open a directory for listing
   */
  static void cvmfs_opendir(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_opendir on inode: %d", ino);

    string path;
    DirectoryEntry d;
    bool found;

    found = get_path_for_inode(ino, &path) && get_dirent_for_inode(ino, &d);

    if (not found) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    if (!d.IsDirectory()) {
      fuse_reply_err(req, ENOTDIR);
      return;
    }

    // build dir listing
    struct dirListingBuffer b;
    memset(&b, 0, sizeof(b));

    // add current directory link
    struct stat info;
    memset(&info, 0, sizeof(info));
    info = d.GetStatStructure();
    addToDirListingBuffer(req, &b, ".", &info);

    // add parent directory link
    DirectoryEntry p;
    if (d.inode() != catalog_manager->GetRootInode() &&
        get_dirent_for_inode(d.parent_inode(), &p)) {
      info = p.GetStatStructure();
      addToDirListingBuffer(req, &b, "..", &info);
    }

    // create directory listing
    DirectoryEntryList dir_listing;
    if (not catalog_manager->Listing(path, &dir_listing)) {
      fuse_reply_err(req, EIO);
      return;
    }

    DirectoryEntryList::const_iterator i, iEnd;
    for (i = dir_listing.begin(), iEnd = dir_listing.end(); i != iEnd; ++i) {
      info = i->GetStatStructure();
      addToDirListingBuffer(req, &b, i->name().c_str(), &info);
    }

    // save the directory listing and return a handle to the listing
    pthread_mutex_lock(&open_dir_listings_mutex);
    open_dir_listings[current_dir_listing_handle] = b;
    fi->fh = current_dir_listing_handle;
    ++current_dir_listing_handle;
    pthread_mutex_unlock(&open_dir_listings_mutex);

    // reply
    fuse_reply_open(req, fi);
  }

	/**
	 * Release a directory (probably called after reading it)
	 */
  static void cvmfs_releasedir(fuse_req_t req, fuse_ino_t ino,
                               struct fuse_file_info *fi) {
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_releasedir on inode: %d",
             catalog_manager->MangleInode(ino));

    // find the directory listing to release and release it
    int reply = 0;
    pthread_mutex_lock(&open_dir_listings_mutex);
    std::map<unsigned int, dirListingBuffer>::iterator openDir =
      open_dir_listings.find(fi->fh);
    if (openDir != open_dir_listings.end()) {
      free(openDir->second.p);
      open_dir_listings.erase(openDir);
      reply = 0;
    } else {
      reply = EIO;
    }
    pthread_mutex_unlock(&open_dir_listings_mutex);

    // reply on request
    fuse_reply_err(req, reply);
  }

	/**
   * Read the directory listing
   */
  static void cvmfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                            off_t off, struct fuse_file_info *fi) {
    LogCvmfs(kLogCvmfs, kLogDebug,
             "cvmfs_readdir on inode %d reading %d bytes from offset %d",
             catalog_manager->MangleInode(ino), size, off);

    // find the directory listing to read
    pthread_mutex_lock(&open_dir_listings_mutex);
    std::map<unsigned int, dirListingBuffer>::const_iterator openDir =
      open_dir_listings.find(fi->fh);
    pthread_mutex_unlock(&open_dir_listings_mutex);

    if (openDir != open_dir_listings.end()) {
      replyBufferLimited(req, openDir->second.p, openDir->second.size, off,
                         size);
    } else {
      fuse_reply_err(req, EIO);
    }
  }

  /**
   * Emulates the getattr walk done by Fuse
   */
  static int walk_path(const string &path) {
    //      struct stat info;
    // if ((path == "") || (path == "/"))
    //    return cvmfs_getattr("/", &info);

    int attr_result = walk_path(GetParentPath(path));
    // if (attr_result == 0)
    //    return cvmfs_getattr(path.c_str(), &info);

    return attr_result;
  }


  /**
   * Removes a file from local cache
   * TODO
   */
  int clear_file(const string &path) {
  /*  int attr_result = walk_path(path);
    if (attr_result != 0)
      return attr_result;

    const hash::t_md5 md5(catalog::mangled_path(path));
    int result;

    catalog::lock();

    catalog::t_dirent d;
    if (catalog::lookup_informed_unprotected(md5, find_catalog_id(path), d)) {
      if ((!(d.flags & catalog::FILE)) || (d.flags & catalog::FILE_LINK)) {
        result = -EINVAL;
      } else {
        lru::remove(d.checksum);
        result = 0;
      }
    } else {
      result = -ENOENT;
    }

    catalog::unlock();

    return result;*/
    return 0;
  }

  static void cvmfs_statfs(fuse_req_t req, fuse_ino_t ino) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_statfs on inode: %d", ino);

    /* If we return 0 it will cause the fs
     to be ignored in "df" */
    struct statvfs info;
    memset(&info, 0, sizeof(info));

    /* Unmanaged cache */
    if (lru::GetCapacity() == 0) {
      fuse_reply_statfs(req, &info);
      return;
    }

    uint64_t available = 0;
    uint64_t size = lru::GetSize();

    info.f_bsize = 1;

    if (lru::GetCapacity() == (uint64_t)(-1)) {
      /* Unrestricted cache, look at free space on cache dir fs */
      struct statfs cache_buf;
      if (statfs(".", &cache_buf) == 0) {
        available = cache_buf.f_bavail * cache_buf.f_bsize;
        info.f_blocks = size + available;
      } else {
        info.f_blocks = size;
      }
    } else {
      /* Take values from LRU module */
      info.f_blocks = lru::GetCapacity();
      available = lru::GetCapacity() - size;
    }

    info.f_bfree = info.f_bavail = available;

    fuse_reply_statfs(req, &info);
    //      return 0;
  }

#ifdef __APPLE__
  static void cvmfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                             size_t size, uint32_t position) {
#else
  static void cvmfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                             size_t size) {
#endif
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug,
             "cvmfs_getxattr on inode: %d for xattr: %s", ino, name);
    const string attr = name;
    DirectoryEntry d;
    bool found;

    found = get_dirent_for_inode(ino, &d);

    if (not found) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    ostringstream message;

    if (attr == "user.pid") {
      message << cvmfs::pid;

    } else if (attr == "user.version") {
      message << VERSION << "." << CVMFS_PATCH_LEVEL;

    } else if (attr == "user.hash") {
      if (!d.checksum().IsNull()) {
        message << d.checksum().ToString() << " (SHA-1)";
      } else {
        fuse_reply_err(req, ENOATTR);
        return;
      }

    } else if (attr == "user.lhash") {
      if (!d.checksum().IsNull()) {
        string result;
        int fd = cache::Open(d.checksum());
        if (fd < 0) {
          message << "Not in cache";
        } else {
          hash::Any hash(hash::kSha1); // TODO
          FILE *f = fdopen(fd, "r");
          if (not f) {
            fuse_reply_err(req, EIO);
            return;
          }

          if (!zlib::CompressFile2Null(f, &hash)) {
            fclose(f);
            fuse_reply_err(req, EIO);
            return;
          }

          fclose(f);
          message << hash.ToString() << " (SHA-1)";
        }
      } else {
        fuse_reply_err(req, ENOATTR);
        return;
      }

    } else if (attr == "user.revision") {
      const uint64_t revision = catalog_manager->GetRevision();

      message << revision;

    } else if (attr == "user.expires") {
      time_t expires = 0;
      // TODO(rene): remove that or implement it properly
      // catalog_manager->GetExpireTime();

      time_t now = time(NULL);
      message << (expires-now)/60;

    } else if (attr == "user.maxfd") {
      message << nofiles-NUM_RESERVED_FD;

    } else if (attr == "user.usedfd") {
      message << atomic_read32(&open_files);

    } else if (attr == "user.nioerr") {
      message << atomic_read32(&nioerr);

    } else if (attr == "user.proxy") {
      vector< vector<string> > proxy_chain;
      unsigned current_group;
      download::GetProxyInfo(&proxy_chain, &current_group);
      if (proxy_chain.size()) {
        message << proxy_chain[current_group][0];
      } else {
        message << "DIRECT";
      }

    } else if (attr == "user.host") {
      vector<string> host_chain;
      vector<int> rtt;
      unsigned current_host;
      download::GetHostInfo(&host_chain, &rtt, &current_host);
      if (host_chain.size()) {
        message << string(host_chain[current_host]);
      } else {
        message << "internal error: no hosts defined";
      }

    } else if (attr == "user.uptime") {
      time_t now = time(NULL);
      uint64_t uptime = now - boot_time;
      /*if (uptime / 60) {
       if (uptime / 3600) {
       if (uptime / 84600) {
       result << uptime/84600 << " days, ";
       }
       result << (uptime / 3600)%24 << " hours, ";
       }
       result << (uptime / 60)%60 << " minutes, ";
       }*/
      message << uptime / 60;

    } else if (attr == "user.nclg") {
      int num = catalog_manager->GetNumberOfAttachedCatalogs();
      message << num;

    } else if (attr == "user.nopen") {
      message << atomic_read64(&nopen);

    } else if (attr == "user.ndownload") {
      message << atomic_read64(&ndownload);

    } else if (attr == "user.timeout") {
      unsigned seconds, seconds_direct;
      download::GetTimeout(&seconds, &seconds_direct);
      message << seconds;

    } else if (attr == "user.timeout_direct") {
      unsigned seconds, seconds_direct;
      download::GetTimeout(&seconds, &seconds_direct);
      message << seconds_direct;

    } else if (attr == "user.rx") {
      int64_t rx = download::GetTransferredBytes();
      message << rx/1024;

    } else if (attr == "user.speed") {
      int64_t rx = download::GetTransferredBytes();
      int64_t time = download::GetTransferTime();
      if (time == 0)
        message << "n/a";
      else
        message << (rx/1024)/time;

    } else {
      fuse_reply_err(req, ENOATTR);
      return;
    }

    string msg = message.str();

    if (size == 0) {
      fuse_reply_xattr(req, msg.length());
    } else if (size >= msg.length()) {
      fuse_reply_buf(req, msg.c_str(), msg.length());
    } else {
      fuse_reply_err(req, ERANGE);
    }
  }

  static void cvmfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_listxattr on inode: %d", ino);
    fuse_reply_err(req, EROFS);
  }

  static void cvmfs_forget(fuse_req_t req, fuse_ino_t ino,
                           unsigned long nlookup) {  // NOLINT(runtime/int)
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_forget on inode: %d", ino);
    fuse_reply_none(req);
  }

  static void cvmfs_access(fuse_req_t req, fuse_ino_t ino, int mask) {
    ino = catalog_manager->MangleInode(ino);
    LogCvmfs(kLogCvmfs, kLogDebug,
             "cvmfs_access on inode: %d asking for R: %s W: %s X: %s", ino,
             ((mask & R_OK) ? "yes" : "no"),
             ((mask & W_OK) ? "yes" : "no"),
             ((mask & X_OK) ? "yes" : "no"));

    DirectoryEntry d;
    bool found;

    found = get_dirent_for_inode(ino, &d);

    if (not found) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    // check access rights for owner (RWX access bits --> shift six left)
    // if write access is requested, we always say no
    unsigned int request = mask << 6;
    if ((mask & W_OK) || (request & d.mode()) != request) {
      fuse_reply_err(req, EACCES);
      return;
    }

    // all okay... let's go
    fuse_reply_err(req, 0);
  }

  /**
   * Do after-daemon() initialization
   */
  static void cvmfs_init(void *userdata, struct fuse_conn_info *conn) {
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_init");
    int retval;

    pid = getpid();

    /* Switch back to cache dir after daemon() */
    retval = chdir(cachedir.c_str());
    assert(retval == 0);

    monitor::Spawn();

    /* Setup Tracer */
    if (tracefile != "")
      tracer::Init(8192, 7000, tracefile);
    else
      tracer::InitNull();

    lru::Spawn();
    talk::spawn();

    download::Spawn();

    //      max_cache_timeout = fuse_get_max_cache_timeout();
  }

  static void cvmfs_destroy(void *unused __attribute__((unused))) {
    LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_destroy");
    tracer::Fini();
  }

  /**
   * Puts the callback functions in one single structure
   */
  static void set_cvmfs_ops(struct fuse_lowlevel_ops *cvmfs_operations) {
    /* Init/Fini */
    cvmfs_operations->init     = cvmfs_init;
    cvmfs_operations->destroy  = cvmfs_destroy;

    /* Implemented */
    cvmfs_operations->lookup      = cvmfs_lookup;
    cvmfs_operations->forget      = cvmfs_forget;
    cvmfs_operations->getattr     = cvmfs_getattr;
    cvmfs_operations->readlink    = cvmfs_readlink;
    cvmfs_operations->open        = cvmfs_open;
    cvmfs_operations->read        = cvmfs_read;
    cvmfs_operations->release     = cvmfs_release;
    cvmfs_operations->opendir     = cvmfs_opendir;
    cvmfs_operations->readdir     = cvmfs_readdir;
    cvmfs_operations->releasedir  = cvmfs_releasedir;
    cvmfs_operations->statfs      = cvmfs_statfs;
    cvmfs_operations->getxattr    = cvmfs_getxattr;
    cvmfs_operations->listxattr   = cvmfs_listxattr;
    cvmfs_operations->access      = cvmfs_access;

    /* Not implemented... just stubs */
    cvmfs_operations->setattr     = cvmfs_setattr;
    cvmfs_operations->mknod       = cvmfs_mknod;
    cvmfs_operations->mkdir       = cvmfs_mkdir;
    cvmfs_operations->unlink      = cvmfs_unlink;
    cvmfs_operations->rmdir       = cvmfs_rmdir;
    cvmfs_operations->symlink     = cvmfs_symlink;
    cvmfs_operations->rename      = cvmfs_rename;
    cvmfs_operations->link        = cvmfs_link;
    cvmfs_operations->write       = cvmfs_write;
    cvmfs_operations->flush       = cvmfs_flush;
    cvmfs_operations->fsync       = cvmfs_fsync;
    cvmfs_operations->fsyncdir    = cvmfs_fsyncdir;
    cvmfs_operations->setxattr    = cvmfs_setxattr;
    cvmfs_operations->removexattr = cvmfs_removexattr;
    cvmfs_operations->create      = cvmfs_create;
    cvmfs_operations->getlk       = cvmfs_getlk;
    cvmfs_operations->setlk       = cvmfs_setlk;
    cvmfs_operations->bmap        = cvmfs_bmap;
    cvmfs_operations->ioctl       = cvmfs_ioctl;
    cvmfs_operations->poll        = cvmfs_poll;
  }
}  // namespace cvmfs



//using namespace cvmfs; //NOLINT

/**
 * One single structure to contain the file system options.
 * Strings(char *) must be deallocated by the user
 */
struct cvmfs_opts {
  unsigned timeout;
  unsigned timeout_direct;
  unsigned max_ttl;
  char     *hostname;
  char     *cachedir;
  char     *proxies;
  char     *tracefile;
  char     *whitelist;
  char     *pubkey;
  char     *logfile;
  char     *deep_mount;
  char     *blacklist;
  char     *repo_name;
  int      force_signing;
  int      rebuild_cachedb;
  int      nofiles;
  int      grab_mountpoint;
  int      syslog_level;
  int      kernel_cache;
  int      auto_cache;
  int      entry_timeout;
  int      attr_timeout;
  int      negative_timeout;
  int      use_ino;

  int64_t  quota_limit;
  int64_t  quota_threshold;
};

/* Follow the fuse convention for option parsing */
enum {
  KEY_HELP,
  KEY_VERSION,
  KEY_FOREGROUND,
  KEY_SINGLETHREAD,
  KEY_DEBUG,
};
#define CVMFS_OPT(t, p, v) { t, offsetof(struct cvmfs_opts, p), v }
#define CVMFS_SWITCH(t, p) { t, offsetof(struct cvmfs_opts, p), 1 }
static struct fuse_opt cvmfs_array_opts[] = {
  CVMFS_OPT("timeout=%u",          timeout, 2),
  CVMFS_OPT("timeout_direct=%u",   timeout_direct, 2),
  CVMFS_OPT("max_ttl=%u",          max_ttl, 0),
  CVMFS_OPT("cachedir=%s",         cachedir, 0),
  CVMFS_OPT("proxies=%s",          proxies, 0),
  CVMFS_OPT("tracefile=%s",        tracefile, 0),
  CVMFS_SWITCH("force_signing",    force_signing),
  CVMFS_OPT("whitelist=%s",        whitelist, 0),
  CVMFS_OPT("pubkey=%s",           pubkey, 0),
  CVMFS_OPT("logfile=%s",          logfile, 0),
  CVMFS_SWITCH("rebuild_cachedb",  rebuild_cachedb),
  CVMFS_OPT("quota_limit=%ld",     quota_limit, 0),
  CVMFS_OPT("quota_threshold=%ld", quota_threshold, 0),
  CVMFS_OPT("nofiles=%d",          nofiles, 0),
  CVMFS_SWITCH("grab_mountpoint",  grab_mountpoint),
  CVMFS_OPT("deep_mount=%s",       deep_mount, 0),
  CVMFS_OPT("repo_name=%s",        repo_name, 0),
  CVMFS_OPT("blacklist=%s",        blacklist, 0),
  CVMFS_OPT("syslog_level=%d",     syslog_level, 3),
  CVMFS_OPT("entry_timeout=%d",    entry_timeout, 60),
  CVMFS_OPT("attr_timeout=%d",     attr_timeout, 60),
  CVMFS_OPT("negative_timeout=%d", negative_timeout, 60),
  CVMFS_SWITCH("use_ino",          entry_timeout),
  CVMFS_SWITCH("kernel_cache",     kernel_cache),
  CVMFS_SWITCH("auto_cache",       auto_cache),

  FUSE_OPT_KEY("-V",            KEY_VERSION),
  FUSE_OPT_KEY("--version",     KEY_VERSION),
  FUSE_OPT_KEY("-h",            KEY_HELP),
  FUSE_OPT_KEY("--help",        KEY_HELP),
  FUSE_OPT_KEY("-f",            KEY_FOREGROUND),
  FUSE_OPT_KEY("-d",            KEY_DEBUG),
  FUSE_OPT_KEY("debug",         KEY_DEBUG),
  FUSE_OPT_KEY("-s",            KEY_SINGLETHREAD),
  {0, 0, 0},
};


struct cvmfs_opts cvmfs_opts;
struct fuse_args fuse_args;

/**
 * Display the usage message.
 * It will be done when we requested (the flag "-h" for example),
 * but also when an unidentified option is found.
 */
static void usage(const char *progname) {
  fprintf(stderr,
          "Copyright (c) 2009- CERN\n"
          "All rights reserved\n\n"
          "Please visit http://cernvm.cern.ch details.\n\n");

  if (progname)
    fprintf(stderr,
            "usage: %s <mountpath> <url>[,<url>]* [options]\n\n", progname);

  fprintf(stderr,
    "where options are:\n"
    " -o opt,[opt...]  mount options\n\n"
    "CernVM-FS options: \n"
    " -o timeout=SECONDS         "
      "Timeout for network operations (default is %d)\n"
    " -o timeout_direct=SECONDS  "
      "Timeout for network operations without proxy (default is %d)\n"
    " -o max_ttl=MINUTES         "
      "Maximum TTL for file catalogs (default: take from catalog)\n"
    " -o cachedir=DIR            Where to store disk cache\n"
    " -o proxies=HTTP_PROXIES    "
      "Set the HTTP proxy list, such as 'proxy1|proxy2;DIRECT'\n"
    " -o tracefile=FILE          Trace FUSE opaerations into FILE\n"
    " -o whitelist=URL           "
      "HTTP location of trusted catalog certificates "
      "(defaults is /.cvmfswhitelist)\n"
    " -o pubkey=PEMFILE          "
      "Public RSA key that is used to verify the whitelist signature.\n"
    " -o force_signing           "
      "Except only signed catalogs\n"
    " -o rebuild_cachedb         "
      "Force rebuilding the quota cache db from cache directory\n"
    " -o quota_limit=MB          "
      "Limit size of data chunks in cache. -1 Means unlimited.\n"
    " -o quota_threshold=MB      Cleanup until size is <= threshold\n"
    " -o nofiles=NUMBER          "
      "Set the maximum number of open files for CernVM-FS process "
      "(soft limit)\n"
    " -o grab_mountpoint         "
      "Give ownership of the mountpoint to the user before mounting "
      "(automount hack)\n"
    " -o logfile=FILE            "
      "Logs all messages to FILE instead of stderr and daemonizes.\n"
      "                            Makes only sense for the debug version\n"
    " -o deep_mount=prefix       "
      "Path prefix if a repository is mounted on a nested catalog,\n"
    "                            i.e. deep_mount=/software/15.0.1\n"
    " -o repo_name=<repository>  "
      "Unique name of the mounted repository, e.g. atlas.cern.ch\n"
    " -o blacklist=FILE          "
      "Local blacklist for invalid certificates.  "
      "Has precedence over the whitelist.\n"
      "                            (Default is /etc/cvmfs/blacklist)\n"
    " -o syslog_level=NUMBER     "
      "Sets the level used for syslog to DEBUG (1), INFO (2), or NOTICE (3).\n"
    "                            Default is NOTICE.\n"
    " Note: you cannot load files greater than quota_limit-quota_threshold\n",
      2, 2);

  /* Print the help from FUSE */
  //   const char *args[] = {progname, "-h"};
  //   static struct fuse_operations op;
  //   fuse_main(2, (char**)args, &op);
}


/**
 * Since certain fileds in cvmfs_opts are filled automatically when parsing it, \
 * we needed a procedure to free the space.
 */
static void free_cvmfs_opts(struct cvmfs_opts *opts) {
  if (opts->hostname)       free(opts->hostname);
  if (opts->cachedir)       free(opts->cachedir);
  if (opts->proxies)        free(opts->proxies);
  if (opts->tracefile)      free(opts->tracefile);
  if (opts->whitelist)      free(opts->whitelist);
  if (opts->pubkey)         free(opts->pubkey);
  if (opts->logfile)        free(opts->logfile);
  if (opts->deep_mount)     free(opts->deep_mount);
  if (opts->blacklist)      free(opts->blacklist);
  if (opts->repo_name)      free(opts->repo_name);
}


/**
 * Checks whether the given option is one of our own options
 * (if it's not, it probably belongs to fuse).
 */
static int is_cvmfs_opt(const char *arg) {
  if (arg[0] != '-') {
    unsigned arglen = strlen(arg);
    const char **o;
    for (o = (const char**)cvmfs_array_opts; *o; o++) {
      unsigned olen = strlen(*o);
      if ((arglen > olen && arg[olen] == '=') &&
          (strncasecmp(arg, *o, olen) == 0))
        return 1;
    }
  }
  return 0;
}


/**
 * The callback used when fuse is parsing all the options
 * We separate CVMFS options from FUSE options here.
 *
 * \return On success zero, else non-zero
 */
static int cvmfs_opt_proc(void *data __attribute__((unused)), const char *arg,
                          int key, struct fuse_args *outargs) {
  switch (key) {
    case FUSE_OPT_KEY_OPT:
      if (is_cvmfs_opt(arg)) {
        /* If this is a "-o" option and is not one of ours, we assume that this
         must be used when mounting fuse not when instanciating the file system.
         It can't be one of our option if it doesnt match the template */
        return 0;
      }
      if (strstr(arg, "uid=")) {
        cvmfs::uid = atoi(arg+4);
        return 0;
      }
      if (strstr(arg, "gid=")) {
        cvmfs::gid = atoi(arg+4);
        return 0;
      }
      return 1;

    case FUSE_OPT_KEY_NONOPT:
      if (!cvmfs_opts.hostname &&
          ((strstr(arg, "http://") == arg) ||
           (strstr(arg, "file://") == arg))) {
        /* If we receive a parameter that contains "http://"
         we know for sure that it's our remote server */
        cvmfs_opts.hostname = strdup(arg);
      } else {
        /* If we receive any other string, we take it as the mount point. */
        cvmfs::mountpoint = arg;
      }
      return 0;

    case KEY_HELP:
      usage(outargs->argv[0]);
      fuse_opt_add_arg(outargs, "-ho");
      exit(0);

    case KEY_VERSION:
      fprintf(stderr, "CernVM-FS version %s\n", PACKAGE_VERSION);
#if FUSE_VERSION >= 25
      fuse_opt_add_arg(outargs, "--version");
#endif
      exit(0);

    case KEY_FOREGROUND:
      fuse_opt_add_arg(outargs, "-f");
      return 0;

    case KEY_SINGLETHREAD:
      fuse_opt_add_arg(outargs, "-s");
      return 0;

    case KEY_DEBUG:
      fuse_opt_add_arg(outargs, "-d");
      return 0;

    default:
      fprintf(stderr, "internal error\n");
      abort();
  }
}


/* Making OpenSSL (libcrypto) thread-safe */
pthread_mutex_t *libcrypto_locks;

static void libcrypto_lock_callback(int mode, int type,
                                    const char *file, int line) {
  (void)file;
  (void)line;

  int retval;

  if (mode & CRYPTO_LOCK) {
    retval = pthread_mutex_lock(&(libcrypto_locks[type]));
  } else {
    retval = pthread_mutex_unlock(&(libcrypto_locks[type]));
  }
  assert(retval == 0);
}

static pthread_t libcrypto_thread_id() {
  return pthread_self();
}

static void libcrypto_mt_setup() {
  libcrypto_locks = static_cast<pthread_mutex_t *>(OPENSSL_malloc(
                      CRYPTO_num_locks() * sizeof(pthread_mutex_t)));
  for (int i = 0; i < CRYPTO_num_locks(); ++i) {
    int retval = pthread_mutex_init(&(libcrypto_locks[i]), NULL);
    assert(retval == 0);
  }

  CRYPTO_set_id_callback(libcrypto_thread_id);
  CRYPTO_set_locking_callback(libcrypto_lock_callback);
}

static void libcrypto_mt_cleanup(void) {
  CRYPTO_set_locking_callback(NULL);
  for (int i = 0; i < CRYPTO_num_locks(); ++i)
    pthread_mutex_destroy(&(libcrypto_locks[i]));

  OPENSSL_free(libcrypto_locks);
}

/**
 * Boot the beast up!
 */
int main(int argc, char *argv[]) {
  int result = 1;
  bool options_ready = false;
  bool download_ready = false;
  bool cache_ready = false;
  bool monitor_ready = false;
  bool signature_ready = false;
  bool quota_ready = false;
  bool catalog_ready = false;
  bool talk_ready = false;

  /* Set a decent umask for new files (no write access to group/everyone).
   We want to allow group write access for the talk-socket. */
  umask(007);

  cvmfs::boot_time = time(NULL);
  cvmfs::prev_io_error.timestamp = 0;
  cvmfs::prev_io_error.delay = 0;

	libcrypto_mt_setup();

  /* Tune SQlite3 memory */
  void *sqlite_scratch = smalloc(8192*16);  // 8 KB for 8 threads
                                            // (2 slots per thread)
  void *sqlite_page_cache = smalloc(1280*3275);  // 4MB
  assert(sqlite3_config(SQLITE_CONFIG_SCRATCH, sqlite_scratch, 8192, 16)
         == SQLITE_OK);
  assert(sqlite3_config(SQLITE_CONFIG_PAGECACHE, sqlite_page_cache, 1280, 3275)
         == SQLITE_OK);
  // 4 KB
  assert(sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 32, 128) == SQLITE_OK);

  /* Catalog memory cache */
  atomic_init32(&cvmfs::cache_inserts);
  atomic_init32(&cvmfs::cache_replaces);
  atomic_init32(&cvmfs::cache_cleans);
  atomic_init32(&cvmfs::cache_hits);
  atomic_init32(&cvmfs::cache_misses);

  atomic_init32(&cvmfs::certificate_hits);
  atomic_init32(&cvmfs::certificate_misses);

  atomic_init64(&cvmfs::nopen);
  atomic_init64(&cvmfs::ndownload);

  struct fuse_chan *ch;
  int err = -1;

  /* Parse options */
  fuse_args.argc = argc;
  fuse_args.argv = argv;
  fuse_args.allocated = 0;
  if ((fuse_opt_parse(&fuse_args, &cvmfs_opts, cvmfs_array_opts,
                      cvmfs_opt_proc) != 0) ||
      !cvmfs_opts.hostname) {
    usage(argv[0]);
    goto cvmfs_cleanup;
    return 1;
  }


  /* Fill cvmfs option variables from Fuse options */
  if (!cvmfs::uid) cvmfs::uid = getuid();
  if (!cvmfs::gid) cvmfs::gid = getgid();
  if (cvmfs_opts.max_ttl) cvmfs::max_ttl = cvmfs_opts.max_ttl*60;
  if (cvmfs_opts.cachedir) cvmfs::cachedir = cvmfs_opts.cachedir;
  if (cvmfs_opts.proxies) cvmfs::proxies = cvmfs_opts.proxies;
  if (cvmfs_opts.force_signing) cvmfs::force_signing = true;
  if (cvmfs_opts.timeout == 0) cvmfs_opts.timeout = 2;
  if (cvmfs_opts.timeout_direct == 0) cvmfs_opts.timeout_direct = 2;
  if (cvmfs_opts.syslog_level == 0) cvmfs_opts.syslog_level = 3;
  if (cvmfs_opts.pubkey) cvmfs::pubkey = cvmfs_opts.pubkey;
  if (cvmfs_opts.tracefile) cvmfs::tracefile = cvmfs_opts.tracefile;
  if (cvmfs_opts.deep_mount)
    cvmfs::deep_mount = MakeCanonicalPath(cvmfs_opts.deep_mount);
  if (cvmfs_opts.blacklist) cvmfs::blacklist = cvmfs_opts.blacklist;
  if (cvmfs_opts.repo_name) cvmfs::repo_name = cvmfs_opts.repo_name;
  if (cvmfs_opts.kernel_cache)
      PrintWarning("using deprecated mount option 'kernel_cache' - ignoring");
  if (cvmfs_opts.auto_cache)
      PrintWarning("using deprecated mount option 'auto_cache' - ignoring");

  /* seperate first host from hostlist */
  unsigned iter_hostname;
  for (iter_hostname = 0; iter_hostname < strlen(cvmfs_opts.hostname);
       ++iter_hostname) {
    if (cvmfs_opts.hostname[iter_hostname] == ',') break;
  }
  if (iter_hostname == 0)
    cvmfs::root_url = "";
  else
    cvmfs::root_url = string(cvmfs_opts.hostname, iter_hostname);

  if (cvmfs_opts.whitelist)
    cvmfs::whitelist = cvmfs_opts.whitelist;
  else
    cvmfs::whitelist = "/.cvmfswhitelist";
  options_ready = true;

  /* Syslog level */
  SetLogSyslogLevel(cvmfs_opts.syslog_level);
  SetLogSyslogPrefix(cvmfs::repo_name);

  /* Maximum number of open files */
  if (cvmfs_opts.nofiles) {
    if (cvmfs_opts.nofiles < 0) {
      cerr << "Failure: number of open files must be a positive number" << endl;
      goto cvmfs_cleanup;
    }
    struct rlimit rpl;
    memset(&rpl, 0, sizeof(rpl));
    getrlimit(RLIMIT_NOFILE, &rpl);
    if (rpl.rlim_max < (unsigned)cvmfs_opts.nofiles)
      rpl.rlim_max = cvmfs_opts.nofiles;
    rpl.rlim_cur = cvmfs_opts.nofiles;
    if (setrlimit(RLIMIT_NOFILE, &rpl) != 0) {
      cerr << "Failed to set maximum number of open files, "
           << "insufficient permissions" << endl;
      goto cvmfs_cleanup;
    }
  }

  /* Grab mountpoint */
  if (cvmfs_opts.grab_mountpoint) {
    if ((chown(cvmfs::mountpoint.c_str(), cvmfs::uid, cvmfs::gid) != 0) ||
        (chmod(cvmfs::mountpoint.c_str(), 0755) != 0)) {
      cerr << "Failed to grab mountpoint (" << errno << ")" << endl;
      goto cvmfs_cleanup;
    }
  }

  /* Set debug log file */
  if (cvmfs_opts.logfile) {
    SetLogDebugFile(string(cvmfs_opts.logfile));
  }

  download::Init(16);
  download_ready = true;

  /* CVMFS has its own proxy environment, chain of proxies */
  download::SetHostChain(string(cvmfs_opts.hostname));
  download::SetProxyChain(string(cvmfs::proxies.c_str()));
  download::SetTimeout(cvmfs_opts.timeout, cvmfs_opts.timeout_direct);

  /* Try to jump to cache directory.  This tests, if it is accassible.
     Also, it brings speed later on. */
  if (!MkdirDeep(cvmfs::cachedir, 0700)) {
    cerr << "Failure: cannot create cache directory " << cvmfs::cachedir
         << endl;
    goto cvmfs_cleanup;
  }

  if (chdir(cvmfs::cachedir.c_str()) != 0) {
    cerr << "Failure: cache directory " << cvmfs::cachedir << " is unavailable"
         << endl;
    goto cvmfs_cleanup;
  }

  /* Try to init the cache... this creates a set of directories in
   cvmfs::cachedir (256 directories named 00..ff) */
  if (!cache::Init(".", cvmfs::root_url)) {
    cerr << "Failed to setup cache in " << cvmfs::cachedir
         << ": " << strerror(errno) << endl;
    goto cvmfs_cleanup;
  }
  cache_ready = true;

  /* Monitor, check for maximum number of open files */
  if (!monitor::Init(".", true)) {
    cerr << "Failed to initialize watchdog." << endl;
    goto cvmfs_cleanup;
  }
  cvmfs::nofiles = monitor::GetMaxOpenFiles();
  atomic_init32(&cvmfs::open_files);
  atomic_init32(&cvmfs::nioerr);
  monitor_ready = true;

  signature::Init();
  if (!signature::LoadPublicRsaKeys(cvmfs_opts.pubkey ? cvmfs_opts.pubkey : "")) {
    LogCvmfs(kLogCvmfs, kLogStderr, "Failed to load public key(s)");
    goto cvmfs_cleanup;
  } else {
    if (!cvmfs_opts.pubkey)
      LogCvmfs(kLogCvmfs, kLogStdout, "Warning: No public master key given. "
               "Cvmfs will fail on signed catalogs!");
    else
      LogCvmfs(kLogCvmfs, kLogStdout, "CernVM-FS: using public key(s) %s",
               JoinStrings(
                 SplitString(cvmfs_opts.pubkey, ':'), ", ").c_str());
  }
  signature_ready = true;

  /* Init quota / lru cache */
  if (cvmfs_opts.quota_limit < 0) {
    LogCvmfs(kLogCvmfs, kLogDebug, "unlimited cache size");
    cvmfs_opts.quota_limit = -1;
    cvmfs_opts.quota_threshold = 0;
  } else {
    cvmfs_opts.quota_limit *= 1024*1024;
    cvmfs_opts.quota_threshold *= 1024*1024;
  }
  if (!lru::Init(".", (uint64_t)cvmfs_opts.quota_limit,
                 (uint64_t)cvmfs_opts.quota_threshold,
                 cvmfs_opts.rebuild_cachedb)) {
    LogCvmfs(kLogCvmfs, kLogStderr, "Failed to initialize lru cache");
    goto cvmfs_cleanup;
  }
  quota_ready = true;

  if (cvmfs_opts.rebuild_cachedb) {
    LogCvmfs(kLogCvmfs, kLogStdout,
             "CernVM-FS: rebuilding lru cache database...");
    if (!lru::BuildDatabase()) {
      LogCvmfs(kLogCvmfs, kLogStderr, "Failed to rebuild lru cache database");
      goto cvmfs_cleanup;
    }
  }
  if (lru::GetSize() > lru::GetCapacity()) {
    LogCvmfs(kLogCvmfs, kLogStdout,
             "Warning: your cache is already beyond quota size, cleaning up");
    if (!lru::Cleanup(cvmfs_opts.quota_threshold)) {
      LogCvmfs(kLogCvmfs, kLogStderr, "Failed to clean up");
      goto cvmfs_cleanup;
    }
  }
  if (cvmfs_opts.quota_limit) {
    LogCvmfs(kLogCvmfs, kLogStdout,
             "CernVM-FS: quota initialized, current size %luMB",
             lru::GetSize()/(1024*1024));
  }

  /* Create the file catalog from the web server */
  // if (!catalog::init(cvmfs::uid, cvmfs::gid)) {
  //    cerr << "Failed to initialize catalog" << endl;
  //    goto cvmfs_cleanup;
  // }
  // err_catalog =
  //  load_and_attach_catalog(cvmfs::root_catalog,
  //                          hash::t_md5(cvmfs::deep_mount), "/", -1, false);
  // pmesg(D_CVMFS, "initial catalog load results in %d", err_catalog);
  // if (err_catalog == -EIO) {
  //    cerr << "Failed to load catalog (IO error)" << endl;
  //    goto cvmfs_cleanup;
  // }
  // if (err_catalog == -EPERM) {
  //    cerr << "Failed to verify catalog signature" << endl;
  //    goto cvmfs_cleanup;
  // }
  // if ((err_catalog == -EINVAL) || (err_catalog == -EAGAIN)) {
  //    cerr << "Failed to load catalog (corrupted data)" << endl;
  //    goto cvmfs_cleanup;
  // }
  // if (err_catalog == -ENOSPC) {
  //    cerr << "Failed to load catalog (no space in cache)" << endl;
  //    goto cvmfs_cleanup;
  // }

  cvmfs::catalog_manager = new
    cvmfs::RemoteCatalogManager(cvmfs::root_url, cvmfs::repo_name,
                                cvmfs::whitelist, cvmfs::blacklist,
                                cvmfs::force_signing);
  if (not cvmfs::catalog_manager->Init()) {
    LogCvmfs(kLogCvmfs, kLogStderr, "Failed to initialize catalog manager");
    goto cvmfs_cleanup;
  }
  catalog_ready = true;

  if (!talk::init(".")) {
    LogCvmfs(kLogCvmfs, kLogStderr, "Failed to initialize talk socket (%d)",
             errno);
    goto cvmfs_cleanup;
  }
  talk_ready = true;

  /* Set fuse callbacks, remove url from arguments */
  LogCvmfs(kLogCvmfs, kLogSyslog,
           "CernVM-FS: linking %s to remote directoy %s",
           cvmfs::mountpoint.c_str(), cvmfs::root_url.c_str());
  struct fuse_lowlevel_ops cvmfs_operations;
  cvmfs::set_cvmfs_ops(&cvmfs_operations);

  cvmfs::inode_cache = new cvmfs::InodeCache(cvmfs::inode_cache_size);
  cvmfs::path_cache = new cvmfs::PathCache(cvmfs::path_cache_size);
  cvmfs::md5path_cache = new cvmfs::Md5PathCache(cvmfs::md5path_cache_size);

  if ((ch = fuse_mount(cvmfs::mountpoint.c_str(), &fuse_args)) != NULL) {
    struct fuse_session *se;

    // if ((cvmfs::uid != 0) || (cvmfs::gid != 0)) {
    //    cout << "CernVM-FS: running with credentials " << cvmfs::uid << ":"
    //         << cvmfs::gid << endl;
    //    if ((setgid(cvmfs::gid) != 0) || (setuid(cvmfs::uid) != 0)) {
    //       cerr << "Failed to drop credentials" << endl;
    //       goto cvmfs_cleanup;
    //    }
    // }

    LogCvmfs(kLogCvmfs, kLogStdout, "CernVM-FS: mounted cvmfs on %s",
             cvmfs::mountpoint.c_str());
    daemon(0, 0);

    se = fuse_lowlevel_new(&fuse_args, &cvmfs_operations,
                           sizeof(cvmfs_operations), NULL);
    if (se != NULL) {
      if (fuse_set_signal_handlers(se) != -1) {
        fuse_session_add_chan(se, ch);
        err = fuse_session_loop_mt(se);
        fuse_remove_signal_handlers(se);
        fuse_session_remove_chan(ch);
      }
      fuse_session_destroy(se);
    }
    fuse_unmount(cvmfs::mountpoint.c_str(), ch);
  }
  fuse_opt_free_args(&fuse_args);

  delete cvmfs::path_cache;
  delete cvmfs::inode_cache;

  delete cvmfs::catalog_manager;

  LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog, "CernVM-FS: unmounted %s (%s)",
           cvmfs::mountpoint.c_str(), cvmfs::root_url.c_str());


 cvmfs_cleanup:
  if (talk_ready) talk::fini();
  if (quota_ready) lru::Fini();
  if (signature_ready) signature::Fini();
  if (cache_ready) cache::Fini();
  if (monitor_ready) monitor::Fini();
  if (download_ready) download::Fini();
  if (options_ready) {
    fuse_opt_free_args(&fuse_args);
    free_cvmfs_opts(&cvmfs_opts);
  }

  free(sqlite_page_cache);
  free(sqlite_scratch);

  libcrypto_mt_cleanup();

  return result;
}
