#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# Test runner for nolibc tests

set -e

trap 'echo Aborting...' 'ERR'

crosstool_version=13.2.0
hostarch=x86_64
nproc=$(( $(nproc) + 2))
cache_dir="${XDG_CACHE_HOME:-"$HOME"/.cache}"
download_location="${cache_dir}/crosstools/"
build_location="$(realpath "${cache_dir}"/nolibc-tests/)"
perform_download=0
test_mode=system
werror=1
llvm=
all_archs=(
	i386 x86_64 x32
	arm64 arm armthumb
	mips32le mips32be mipsn32le mipsn32be mips64le mips64be
	ppc ppc64 ppc64le
	riscv32 riscv64
	s390x s390
	loongarch
	sparc32 sparc64
	m68k
	sh4
)
archs="${all_archs[@]}"

TEMP=$(getopt -o 'j:d:c:b:a:m:pelh' -n "$0" -- "$@")

eval set -- "$TEMP"
unset TEMP

print_usage() {
	cat <<EOF
Run nolibc testsuite for multiple architectures with crosstools

Usage:
 $0 [options] <architectures>

Known architectures:
 ${archs}

Options:
 -j [N]         Allow N jobs at once (default: ${nproc})
 -p             Allow download of toolchains
 -d [DIR]       Download location for toolchains (default: ${download_location})
 -c [VERSION]   Version of toolchains to use (default: ${crosstool_version})
 -a [ARCH]      Host architecture of toolchains to use (default: ${hostarch})
 -b [DIR]       Build location (default: ${build_location})
 -m [MODE]      Test mode user/system (default: ${test_mode})
 -e             Disable -Werror
 -l             Build with LLVM/clang
EOF
}

while true; do
	case "$1" in
		'-j')
			nproc="$2"
			shift 2; continue ;;
		'-p')
			perform_download=1
			shift; continue ;;
		'-d')
			download_location="$2"
			shift 2; continue ;;
		'-c')
			crosstool_version="$2"
			shift 2; continue ;;
		'-a')
			hostarch="$2"
			shift 2; continue ;;
		'-b')
			build_location="$(realpath "$2")"
			shift 2; continue ;;
		'-m')
			test_mode="$2"
			shift 2; continue ;;
		'-e')
			werror=0
			shift; continue ;;
		'-l')
			llvm=1
			shift; continue ;;
		'-h')
			print_usage
			exit 0
			;;
		'--')
			shift; break ;;
		*)
			echo 'Internal error!' >&2; exit 1 ;;
	esac
done

if [[ -n "$*" ]]; then
	archs="$*"
fi

crosstool_arch() {
	case "$1" in
	arm64) echo aarch64;;
	armthumb) echo arm;;
	ppc) echo powerpc;;
	ppc64) echo powerpc64;;
	ppc64le) echo powerpc64;;
	riscv) echo riscv64;;
	loongarch) echo loongarch64;;
	mips*) echo mips;;
	s390*) echo s390;;
	sparc*) echo sparc64;;
	x32*) echo x86_64;;
	*) echo "$1";;
	esac
}

crosstool_abi() {
	case "$1" in
	arm | armthumb) echo linux-gnueabi;;
	*) echo linux;;
	esac
}

download_crosstool() {
	arch="$(crosstool_arch "$1")"
	abi="$(crosstool_abi "$1")"

	archive_name="${hostarch}-gcc-${crosstool_version}-nolibc-${arch}-${abi}.tar.gz"
	url="https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/${hostarch}/${crosstool_version}/${archive_name}"
	archive="${download_location}${archive_name}"
	stamp="${archive}.stamp"

	[ -f "${stamp}" ] && return

	echo "Downloading crosstools ${arch} ${crosstool_version}"
	mkdir -p "${download_location}"
	curl -o "${archive}" --fail --continue-at - "${url}"
	tar -C "${download_location}" -xf "${archive}"
	touch "${stamp}"
}

# capture command output, print it on failure
# mimics chronic(1) from moreutils
function swallow_output() {
	if ! OUTPUT="$("$@" 2>&1)"; then
		echo "$OUTPUT"
		return 1
	fi
	return 0
}

test_arch() {
	arch=$1
	ct_arch=$(crosstool_arch "$arch")
	ct_abi=$(crosstool_abi "$1")

	if [ ! -d "${download_location}gcc-${crosstool_version}-nolibc/${ct_arch}-${ct_abi}/bin/." ]; then
		echo "No toolchain found in ${download_location}gcc-${crosstool_version}-nolibc/${ct_arch}-${ct_abi}."
		echo "Did you install the toolchains or set the correct arch ? Rerun with -h for help."
		return 1
	fi

	cross_compile=$(realpath "${download_location}gcc-${crosstool_version}-nolibc/${ct_arch}-${ct_abi}/bin/${ct_arch}-${ct_abi}-")
	build_dir="${build_location}/${arch}"
	if [ "$werror" -ne 0 ]; then
		CFLAGS_EXTRA="$CFLAGS_EXTRA -Werror"
	fi
	MAKE=(make -f Makefile.nolibc -j"${nproc}" XARCH="${arch}" CROSS_COMPILE="${cross_compile}" LLVM="${llvm}" O="${build_dir}")

	case "$test_mode" in
		'system')
			test_target=run
			;;
		'user')
			test_target=run-user
			;;
		*)
			echo "Unknown mode $test_mode"
			exit 1
	esac
	printf '%-15s' "$arch:"
	if [ "$arch" = "s390" ] && ([ "$llvm" = "1" ] || [ "$test_mode" = "user" ]); then
		echo "Unsupported configuration"
		return
	fi
	if [ "$arch" = "m68k" -o "$arch" = "sh4" ] && [ "$llvm" = "1" ]; then
		echo "Unsupported configuration"
		return
	fi
	if [ "$arch" = "x32" ] && [ "$test_mode" = "user" ]; then
		echo "Unsupported configuration"
		return
	fi

	mkdir -p "$build_dir"
	swallow_output "${MAKE[@]}" defconfig
	swallow_output "${MAKE[@]}" CFLAGS_EXTRA="$CFLAGS_EXTRA" "$test_target" V=1
	cp run.out run.out."${arch}"
	"${MAKE[@]}" report | grep passed
}

if [ "$perform_download" -ne 0 ]; then
	for arch in $archs; do
		download_crosstool "$arch"
	done
fi

for arch in $archs; do
	test_arch "$arch"
done
