/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_CATALOG_COUNTERS_H_
#define CVMFS_CATALOG_COUNTERS_H_

#include <stdint.h>
#include <map>
#include <string>
#include <gtest/gtest_prod.h>

namespace swissknife {
  class CommandCheck;
}

namespace catalog {

class DirectoryEntry;
class Database;

// FieldT is either int64_t (DeltaCounters) or uint64_t (Counters)
template<typename FieldT>
class TreeCountersBase {
  friend class swissknife::CommandCheck;
  FRIEND_TEST(T_CatalogCounters, FieldsCombinations);
  FRIEND_TEST(T_CatalogCounters, FieldsMap);

 protected:
  typedef std::map<std::string, const FieldT*> FieldsMap;
  struct Fields {
    Fields() : regular_files(0), symlinks(0), directories(0),
               nested_catalogs(0), chunked_files(0), file_chunks(0),
               file_size(0), chunked_file_size(0) {}

    // typname U is another TreeCountersBase (eg: add DeltaCounters to Counters)

    template<typename U>
    void Add(const U &other) {
      Combine<U, 1>(other);
    }

    template<typename U>
    void Subtract(const U &other) {
      Combine<U, -1>(other);
    }

    template<typename U, int factor>
    void Combine(const U &other) {
      regular_files     += factor * other.regular_files;
      symlinks          += factor * other.symlinks;
      directories       += factor * other.directories;
      nested_catalogs   += factor * other.nested_catalogs;
      chunked_files     += factor * other.chunked_files;
      file_chunks       += factor * other.file_chunks;
      file_size         += factor * other.file_size;
      chunked_file_size += factor * other.chunked_file_size;
    }

    void FillFieldsMap(FieldsMap &map, const std::string &prefix) const {
      map[prefix + "regular"]      = &regular_files;
      map[prefix + "symlink"]      = &symlinks;
      map[prefix + "dir"]          = &directories;
      map[prefix + "nested"]       = &nested_catalogs;
      map[prefix + "chunked"]      = &chunked_files;
      map[prefix + "chunks"]       = &file_chunks;
      map[prefix + "file_size"]    = &file_size;
      map[prefix + "chunked_size"] = &chunked_file_size;
    }

    FieldT regular_files;
    FieldT symlinks;
    FieldT directories;
    FieldT nested_catalogs;
    FieldT chunked_files;
    FieldT file_chunks;
    FieldT file_size;
    FieldT chunked_file_size;
  };

 public:
  bool ReadFromDatabase(const Database   &database);
  bool WriteToDatabase(const Database    &database) const;
  bool InsertIntoDatabase(const Database &database) const;

  void SetZero();

 protected:
  FieldsMap GetFieldsMap() const;

 public:
  Fields self;
  Fields subtree;
};


typedef int64_t DeltaCounters_t;
class DeltaCounters : public TreeCountersBase<DeltaCounters_t> {
  friend class Counters;

 public:
  void PopulateToParent(DeltaCounters &parent) const;
  void Increment(const DirectoryEntry &dirent) { ApplyDelta(dirent,  1); }
  void Decrement(const DirectoryEntry &dirent) { ApplyDelta(dirent, -1); }

 private:
  void ApplyDelta(const DirectoryEntry &dirent, const int delta);
};


typedef uint64_t Counters_t;
class Counters : public TreeCountersBase<Counters_t> {
 public:
  void ApplyDelta(const DeltaCounters &delta);
  void AddAsSubtree(DeltaCounters &delta) const;
  void MergeIntoParent(DeltaCounters &parent_delta) const;
  Counters_t GetSelfEntries() const;
  Counters_t GetSubtreeEntries() const;
  Counters_t GetAllEntries() const;
};

}  // namespace catalog

#include "catalog_counters_impl.h"

#endif  // CVMFS_CATALOG_COUNTERS_H_
