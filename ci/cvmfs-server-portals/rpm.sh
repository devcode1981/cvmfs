#!/bin/sh

#
# This script builds the RPM packages of CernVM-FS server portals add-ons.
#

set -e

SCRIPT_LOCATION=$(cd "$(dirname "$0")"; pwd)
. ${SCRIPT_LOCATION}/../common.sh

if [ $# -lt 2 ]; then
  echo "Usage: $0 <directory with checked out sources> <build result location>"
  echo "This script builds CernVM-FS server portals add-on package"
  exit 1
fi

CVMFS_SOURCE_LOCATION="$1"
ALL_SOURCE_LOCATION="${CVMFS_SOURCE_LOCATION}/.."
CVMFS_RESULT_LOCATION="$2"

rpm_infra_dirs="BUILD RPMS SOURCES SRPMS TMP"
rpm_src_dir="${CVMFS_SOURCE_LOCATION}/packaging/rpm"
spec_file="cvmfs-server-portals.spec"

# sanity checks
[ -d ${ALL_SOURCE_LOCATION}/minio ] || die "minio sources missing"
for d in $rpm_infra_dirs; do
  [ ! -d ${CVMFS_RESULT_LOCATION}/${d} ] || die "build directory seems to be used before (${CVMFS_RESULT_LOCATION}/${d} exists)"
done
[ -f ${rpm_src_dir}/${spec_file} ] || die "$spec_file missing"

echo "preparing build environment in '${CVMFS_RESULT_LOCATION}'..."
for d in $rpm_infra_dirs; do
  mkdir ${CVMFS_RESULT_LOCATION}/${d}
done

echo "preparing sources in '${CVMFS_RESULT_LOCATION}/SOURCES'..."
shuttle_version=$(cat ${rpm_src_dir}/${spec_file} | grep shuttle_version | grep ^%define | awk '{print $3}')
shuttle_commitid=$(cd ${CVMFS_SOURCE_LOCATION} && git rev-parse HEAD)
(cd ${CVMFS_SOURCE_LOCATION} && \
  git archive --format=tar --prefix=cvmfs-shuttle-${shuttle_version}/ \
    -o ${CVMFS_RESULT_LOCATION}/SOURCES/cvmfs-shuttle-${shuttle_version}.tar.gz HEAD)
minio_tag=$(cat ${rpm_src_dir}/${spec_file} | grep minio_tag | grep ^%define | awk '{print $3}')
minio_commitid=$(cd ${ALL_SOURCE_LOCATION}/minio && git rev-parse ${minio_tag})
(cd ${ALL_SOURCE_LOCATION}/minio && \
  git archive --format=tar --prefix=cvmfs-minio-${minio_tag}/ \
    -o ${CVMFS_RESULT_LOCATION}/SOURCES/${minio_tag}.tar.gz ${minio_tag})

echo "Building!"
cd $CVMFS_RESULT_LOCATION
rpmbuild --define="_topdir $CVMFS_RESULT_LOCATION"        \
         --define="_tmppath ${CVMFS_RESULT_LOCATION}/TMP" \
         --define="shuttle_commitid ${shuttle_commitid}"    \
         --define="minio_commitid ${minio_commitid}"      \
         -ba ${rpm_src_dir}/$spec_file

