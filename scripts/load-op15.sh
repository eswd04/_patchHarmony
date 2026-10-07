#!/system/bin/sh
# Load the tested OnePlus 15 module set; pass its directory as the first argument.
set -eu

fail() {
	echo "ERROR: $*" >&2
	exit 1
}

if [ "$#" -gt 1 ]; then
	fail "Usage: sh $0 [module-directory]"
fi
[ "$(id -u)" = 0 ] || fail "Run this script from a root shell (su)."
expected_kernel=6.12.23-android16-5-g188c695beb6d-ab14367805-4k
kernel=$(uname -r)
[ "$kernel" = "$expected_kernel" ] || fail "Kernel $kernel differs from the tested kernel $expected_kernel."

module_dir=${1:-$(dirname "$0")}
module_dir=$(CDPATH= cd -- "$module_dir" && pwd) || fail "Cannot open module directory."
command -v insmod >/dev/null 2>&1 || fail "insmod is unavailable."
for module in droid_lkm droid_lkm_compat droid_lkm_misc; do
	[ -r "$module_dir/$module.ko" ] || fail "Missing module: $module_dir/$module.ko"
done

# Check all existing modules before loading anything. Re-running is allowed only
# when their parameters match; never unload modules to replace them automatically.
check_parameter() {
	module=$1
	parameter=$2
	expected=$3
	[ -d "/sys/module/$module" ] || return 0
	path="/sys/module/$module/parameters/$parameter"
	[ -r "$path" ] || fail "Cannot verify loaded $module: $parameter is unavailable."
	value=$(cat "$path")
	case "$expected:$value" in
		0:0|0:N|0:n|1:1|1:Y|1:y) return 0 ;;
	esac
	fail "$module is already loaded with $parameter=$value; expected $expected. Reboot before loading the tested set."
}

check_parameter droid_lkm gate 0
check_parameter droid_lkm_compat inline_hook 1
check_parameter droid_lkm_compat ghost 1
check_parameter droid_lkm_misc userns 1
check_parameter droid_lkm_misc xt 0
check_parameter droid_lkm_misc devtmpfs 0
check_parameter droid_lkm_misc verbose 1

load_module() {
	module=$1
	shift
	if [ -d "/sys/module/$module" ]; then
		echo "SKIP: $module is already loaded with matching parameters."
		return 0
	fi
	echo "LOAD: $module $*"
	if ! insmod "$module_dir/$module.ko" "$@"; then
		echo "Loading stopped at $module; earlier modules remain loaded." >&2
		echo "Read the kernel log with: dmesg | tail -n 100" >&2
		exit 1
	fi
}

load_module droid_lkm gate=0
load_module droid_lkm_compat inline_hook=1 ghost=1
load_module droid_lkm_misc userns=1 xt=0 devtmpfs=0 verbose=1

status_file=/proc/droid_lkm_misc/status
[ -r "$status_file" ] || fail "Modules loaded, but $status_file is unavailable; inspect dmesg."
cat "$status_file"
grep -Eq '^userns[[:space:]]+ready([[:space:]]|$)' "$status_file" || fail "User NS did not report ready; inspect dmesg."
echo "OK: all three modules are loaded and User NS reports ready."
