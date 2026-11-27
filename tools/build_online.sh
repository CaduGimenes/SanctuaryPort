#!/usr/bin/env bash
# Builds the online module (gpu/online) as out/online/libsanctuarynet.so. It vendors shadPS4's
# network, NP and shadNet libraries and adds Diablo III's private PSN/Demonware hooks; the CPU
# runtime loads it only when SANCTUARY_ONLINE=1 / BB_ONLINE=1.
# Linux build deps: g++, protoc, libprotobuf, OpenSSL, zlib, fmt, SDL3, Zydis.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

for tool in g++ protoc; do command -v "$tool" >/dev/null || { echo "Online module: $tool missing; skipping" >&2; exit 0; }; done
for lib in protobuf openssl zlib fmt sdl3; do
    pkg-config --exists "$lib" 2>/dev/null || { echo "Online module: $lib development files missing; skipping" >&2; exit 0; }
done
ldconfig -p 2>/dev/null | grep -q 'libZydis\.so' || { echo "Online module: Zydis missing; skipping" >&2; exit 0; }

root=gpu/online
obj=out/online/obj
mkdir -p "$obj" out/online
CXX=${CXX:-g++}
FLAGS=(-std=c++23 -O2 -fPIC -DBOOST_ASIO_STANDALONE -DNDEBUG
       -DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_ZLIB_SUPPORT -DBBNET_PORT_VERSION='"0.7"'
       -Wno-deprecated-declarations -Wno-missing-field-initializers)
INC=(-I "$root/bbnet20/include" -I "$root/bbnet20/compat" -I "$root/bbnet20"
     -I "$root/bbnet20/vendor/shadps4" -I "$root/bbnet20/vendor/shadps4/shadnet"
     -I out/online -I "$root" -I "$root/third_party" -I "$root/third_party/httplib"
     -I gpu/third_party/magic_enum/include)

proto=out/online/shadnet.pb.cc
if [[ ! -f $proto || $root/bbnet20/vendor/shadps4/shadnet/shadnet.proto -nt $proto ]]; then
    protoc --proto_path="$root/bbnet20/vendor/shadps4/shadnet" --cpp_out=out/online \
        "$root/bbnet20/vendor/shadps4/shadnet/shadnet.proto"
fi

sources=()
while IFS= read -r f; do sources+=("$f"); done < <(
    find "$root/bbnet20/vendor/shadps4/core/libraries/net" \
         "$root/bbnet20/vendor/shadps4/core/libraries/network" \
         "$root/bbnet20/vendor/shadps4/core/libraries/np" \
         "$root/bbnet20/vendor/shadps4/core/net" \
         "$root/bbnet20/vendor/shadps4/shadnet" -name '*.cpp' |
    grep -vE 'np_trophy\.cpp|trophy_ui\.cpp|np_sns_facebook_dialog\.cpp|np_profile_dialog(_ui)?\.cpp|np_commerce(_ui)?\.cpp')
sources+=("$root/bbnet20/bbnet_glue.cpp" "$root/bbnet20/d3/auth_hooks.cpp"
          "$root/bbnet20/d3/invitations.cpp" "$root/bbnet20/bbsc_stubs.cpp"
          "$root/stubs.cpp" "$root/bbnet20/compat/common/string_util.cpp")
while IFS= read -r f; do sources+=("$f"); done < <(find "$root/common" -maxdepth 2 -name '*.cpp')
sources+=("$proto")

objects=()
for f in "${sources[@]}"; do
    o="$obj/$(printf '%s' "$f" | tr '/' '_').o"
    objects+=("$o")
    if [[ ! -f $o || $f -nt $o ]]; then
        "$CXX" "${FLAGS[@]}" "${INC[@]}" -c "$f" -o "$o"
    fi
done
"$CXX" -shared -o out/online/libsanctuarynet.so "${objects[@]}" \
    -Wl,-Bsymbolic -Wl,--exclude-libs,ALL \
    $(pkg-config --libs protobuf openssl zlib fmt sdl3) -lZydis -lpthread -ldl
echo "Built $PWD/out/online/libsanctuarynet.so"
