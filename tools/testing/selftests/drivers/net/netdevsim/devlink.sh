#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

lib_dir=$(dirname $0)/../../../net/forwarding

ALL_TESTS="fw_flash_test params_test regions_test reload_test \
	   netns_reload_test resource_test dev_info_test \
	   empty_reporter_test dummy_reporter_test rate_test"
NUM_NETIFS=0
source $lib_dir/lib.sh

BUS_ADDR=10
PORT_COUNT=4
VF_COUNT=4
DEV_NAME=netdevsim$BUS_ADDR
SYSFS_NET_DIR=/sys/bus/netdevsim/devices/$DEV_NAME/net/
DEBUGFS_DIR=/sys/kernel/debug/netdevsim/$DEV_NAME/
DL_HANDLE=netdevsim/$DEV_NAME

wait_for_devlink()
{
	"$@" | grep -q $DL_HANDLE
}

devlink_wait()
{
	local timeout=$1

	busywait "$timeout" wait_for_devlink devlink dev
}

fw_flash_test()
{
	DUMMYFILE=$(find /lib/firmware -type f -printf '%P\n' | head -1)
	RET=0

	if [ -z "$DUMMYFILE" ]
	then
		echo "SKIP: unable to find suitable dummy firmware file"
		return
	fi

	echo "10"> $DEBUGFS_DIR/fw_update_flash_chunk_time_ms

	devlink dev flash $DL_HANDLE file $DUMMYFILE
	check_err $? "Failed to flash with status updates on"

	devlink dev flash $DL_HANDLE file $DUMMYFILE component fw.mgmt
	check_err $? "Failed to flash with component attribute"

	devlink dev flash $DL_HANDLE file $DUMMYFILE overwrite settings
	check_fail $? "Flash with overwrite settings should be rejected"

	echo "1"> $DEBUGFS_DIR/fw_update_overwrite_mask
	check_err $? "Failed to change allowed overwrite mask"

	devlink dev flash $DL_HANDLE file $DUMMYFILE overwrite settings
	check_err $? "Failed to flash with settings overwrite enabled"

	devlink dev flash $DL_HANDLE file $DUMMYFILE overwrite identifiers
	check_fail $? "Flash with overwrite settings should be identifiers"

	echo "3"> $DEBUGFS_DIR/fw_update_overwrite_mask
	check_err $? "Failed to change allowed overwrite mask"

	devlink dev flash $DL_HANDLE file $DUMMYFILE overwrite identifiers overwrite settings
	check_err $? "Failed to flash with settings and identifiers overwrite enabled"

	echo "n"> $DEBUGFS_DIR/fw_update_status
	check_err $? "Failed to disable status updates"

	devlink dev flash $DL_HANDLE file $DUMMYFILE
	check_err $? "Failed to flash with status updates off"

	log_test "fw flash test"
}

param_get()
{
	local name=$1

	cmd_jq "devlink dev param show $DL_HANDLE name $name -j" \
	       '.[][][].values[] | select(.cmode == "driverinit").value'
}

param_set()
{
	local name=$1
	local value=$2

	devlink dev param set $DL_HANDLE name $name cmode driverinit value $value
}

check_value()
{
	local name=$1
	local phase_name=$2
	local expected_param_value=$3
	local expected_debugfs_value=$4
	local value

	value=$(param_get $name)
	check_err $? "Failed to get $name param value"
	[ "$value" == "$expected_param_value" ]
	check_err $? "Unexpected $phase_name $name param value"
	value=$(<$DEBUGFS_DIR/$name)
	check_err $? "Failed to get $name debugfs value"
	[ "$value" == "$expected_debugfs_value" ]
	check_err $? "Unexpected $phase_name $name debugfs value"
}

params_test()
{
	RET=0

	local max_macs
	local test1

	check_value max_macs initial 32 32
	check_value test1 initial true Y

	param_set max_macs 16
	check_err $? "Failed to set max_macs param value"
	param_set test1 false
	check_err $? "Failed to set test1 param value"

	check_value max_macs post-set 16 32
	check_value test1 post-set false Y

	devlink dev reload $DL_HANDLE

	check_value max_macs post-reload 16 16
	check_value test1 post-reload false N

	log_test "params test"
}

check_region_size()
{
	local name=$1
	local size

	size=$(devlink region show $DL_HANDLE/$name -j | jq -e -r '.[][].size')
	check_err $? "Failed to get $name region size"
	[ $size -eq 32768 ]
	check_err $? "Invalid $name region size"
}

check_region_snapshot_count()
{
	local name=$1
	local phase_name=$2
	local expected_count=$3
	local count

	count=$(devlink region show $DL_HANDLE/$name -j | jq -e -r '.[][].snapshot | length')
	[ $count -eq $expected_count ]
	check_err $? "Unexpected $phase_name snapshot count"
}

regions_test()
{
	RET=0

	local count

	check_region_size dummy
	check_region_snapshot_count dummy initial 0

	echo ""> $DEBUGFS_DIR/take_snapshot
	check_err $? "Failed to take first dummy region snapshot"
	check_region_snapshot_count dummy post-first-snapshot 1

	echo ""> $DEBUGFS_DIR/take_snapshot
	check_err $? "Failed to take second dummy region snapshot"
	check_region_snapshot_count dummy post-second-snapshot 2

	echo ""> $DEBUGFS_DIR/take_snapshot
	check_err $? "Failed to take third dummy region snapshot"
	check_region_snapshot_count dummy post-third-snapshot 3

	devlink region del $DL_HANDLE/dummy snapshot 1
	check_err $? "Failed to delete first dummy region snapshot"

	check_region_snapshot_count dummy post-first-delete 2

	devlink region new $DL_HANDLE/dummy snapshot 25
	check_err $? "Failed to create a new snapshot with id 25"

	check_region_snapshot_count dummy post-first-request 3

	devlink region dump $DL_HANDLE/dummy snapshot 25 >> /dev/null
	check_err $? "Failed to dump snapshot with id 25"

	devlink region read $DL_HANDLE/dummy snapshot 25 addr 0 len 1 >> /dev/null
	check_err $? "Failed to read snapshot with id 25 (1 byte)"

	devlink region read $DL_HANDLE/dummy snapshot 25 addr 128 len 128 >> /dev/null
	check_err $? "Failed to read snapshot with id 25 (128 bytes)"

	devlink region read $DL_HANDLE/dummy snapshot 25 addr 128 len $((1<<32)) >> /dev/null
	check_err $? "Failed to read snapshot with id 25 (oversized)"

	devlink region read $DL_HANDLE/dummy snapshot 25 addr $((1<<32)) len 128 >> /dev/null 2>&1
	check_fail $? "Bad read of snapshot with id 25 did not fail"

	devlink region del $DL_HANDLE/dummy snapshot 25
	check_err $? "Failed to delete snapshot with id 25"

	check_region_snapshot_count dummy post-second-delete 2

	sid=$(devlink -j region new $DL_HANDLE/dummy | jq '.[][][][]')
	check_err $? "Failed to create a new snapshot with id allocated by the kernel"

	check_region_snapshot_count dummy post-first-request 3

	devlink region dump $DL_HANDLE/dummy snapshot $sid >> /dev/null
	check_err $? "Failed to dump a snapshot with id allocated by the kernel"

	devlink region del $DL_HANDLE/dummy snapshot $sid
	check_err $? "Failed to delete snapshot with id allocated by the kernel"

	check_region_snapshot_count dummy post-first-request 2

	log_test "regions test"
}

reload_test()
{
	RET=0

	devlink dev reload $DL_HANDLE
	check_err $? "Failed to reload"

	echo "y"> $DEBUGFS_DIR/fail_reload
	check_err $? "Failed to setup devlink reload to fail"

	devlink dev reload $DL_HANDLE
	check_fail $? "Unexpected success of devlink reload"

	echo "n"> $DEBUGFS_DIR/fail_reload
	check_err $? "Failed to setup devlink reload not to fail"

	devlink dev reload $DL_HANDLE
	check_err $? "Failed to reload after set not to fail"

	echo "y"> $DEBUGFS_DIR/dont_allow_reload
	check_err $? "Failed to forbid devlink reload"

	devlink dev reload $DL_HANDLE
	check_fail $? "Unexpected success of devlink reload"

	echo "n"> $DEBUGFS_DIR/dont_allow_reload
	check_err $? "Failed to re-enable devlink reload"

	devlink dev reload $DL_HANDLE
	check_err $? "Failed to reload after re-enable"

	log_test "reload test"
}

netns_reload_test()
{
	RET=0

	ip netns add testns1
	check_err $? "Failed add netns \"testns1\""
	ip netns add testns2
	check_err $? "Failed add netns \"testns2\""

	devlink dev reload $DL_HANDLE netns testns1
	check_err $? "Failed to reload into netns \"testns1\""

	devlink -N testns1 dev reload $DL_HANDLE netns testns2
	check_err $? "Failed to reload from netns \"testns1\" into netns \"testns2\""

	ip netns del testns2
	ip netns del testns1

	# Wait until netns async cleanup is done.
	devlink_wait 2000

	log_test "netns reload test"
}

DUMMYDEV="dummytest"

res_val_get()
{
	local netns=$1
	local parentname=$2
	local name=$3
	local type=$4

	cmd_jq "devlink -N $netns resource show $DL_HANDLE -j" \
	       ".[][][] | select(.name == \"$parentname\").resources[] \
	        | select(.name == \"$name\").$type"
}

resource_test()
{
	RET=0

	ip netns add testns1
	check_err $? "Failed add netns \"testns1\""
	ip netns add testns2
	check_err $? "Failed add netns \"testns2\""

	devlink dev reload $DL_HANDLE netns testns1
	check_err $? "Failed to reload into netns \"testns1\""

	# Create dummy dev to add the address and routes on.

	ip -n testns1 link add name $DUMMYDEV type dummy
	check_err $? "Failed create dummy device"
	ip -n testns1 link set $DUMMYDEV up
	check_err $? "Failed bring up dummy device"
	ip -n testns1 a a 192.0.1.1/24 dev $DUMMYDEV
	check_err $? "Failed add an IP address to dummy device"

	local occ=$(res_val_get testns1 IPv4 fib occ)
	local limit=$((occ+1))

	# Set fib size limit to handle one another route only.

	devlink -N testns1 resource set $DL_HANDLE path IPv4/fib size $limit
	check_err $? "Failed to set IPv4/fib resource size"
	local size_new=$(res_val_get testns1 IPv4 fib size_new)
	[ "$size_new" -eq "$limit" ]
	check_err $? "Unexpected \"size_new\" value (got $size_new, expected $limit)"

	devlink -N testns1 dev reload $DL_HANDLE
	check_err $? "Failed to reload"
	local size=$(res_val_get testns1 IPv4 fib size)
	[ "$size" -eq "$limit" ]
	check_err $? "Unexpected \"size\" value (got $size, expected $limit)"

	# Insert 2 routes, the first is going to be inserted,
	# the second is expected to fail to be inserted.

	ip -n testns1 r a 192.0.2.0/24 via 192.0.1.2
	check_err $? "Failed to add route"

	ip -n testns1 r a 192.0.3.0/24 via 192.0.1.2
	check_fail $? "Unexpected successful route add over limit"

	# Now create another dummy in second network namespace and
	# insert two routes. That is over the limit of the netdevsim
	# instance in the first namespace. Move the netdevsim instance
	# into the second namespace and expect it to fail.

	ip -n testns2 link add name $DUMMYDEV type dummy
	check_err $? "Failed create dummy device"
	ip -n testns2 link set $DUMMYDEV up
	check_err $? "Failed bring up dummy device"
	ip -n testns2 a a 192.0.1.1/24 dev $DUMMYDEV
	check_err $? "Failed add an IP address to dummy device"
	ip -n testns2 r a 192.0.2.0/24 via 192.0.1.2
	check_err $? "Failed to add route"
	ip -n testns2 r a 192.0.3.0/24 via 192.0.1.2
	check_err $? "Failed to add route"

	devlink -N testns1 dev reload $DL_HANDLE netns testns2
	check_fail $? "Unexpected successful reload from netns \"testns1\" into netns \"testns2\""

	devlink -N testns2 resource set $DL_HANDLE path IPv4/fib size ' -1'
	check_err $? "Failed to reset IPv4/fib resource size"

	devlink -N testns2 dev reload $DL_HANDLE netns 1
	check_err $? "Failed to reload devlink back"

	ip netns del testns2
	ip netns del testns1

	# Wait until netns async cleanup is done.
	devlink_wait 2000

	log_test "resource test"
}

info_get()
{
	local name=$1

	cmd_jq "devlink dev info $DL_HANDLE -j" ".[][][\"$name\"]" "-e"
}

dev_info_test()
{
	RET=0

	driver=$(info_get "driver")
	check_err $? "Failed to get driver name"
	[ "$driver" == "netdevsim" ]
	check_err $? "Unexpected driver name $driver"

	log_test "dev_info test"
}

empty_reporter_test()
{
	RET=0

	devlink health show $DL_HANDLE reporter empty >/dev/null
	check_err $? "Failed show empty reporter"

	devlink health dump show $DL_HANDLE reporter empty >/dev/null
	check_err $? "Failed show dump of empty reporter"

	devlink health diagnose $DL_HANDLE reporter empty >/dev/null
	check_err $? "Failed diagnose empty reporter"

	devlink health recover $DL_HANDLE reporter empty
	check_err $? "Failed recover empty reporter"

	log_test "empty reporter test"
}

check_reporter_info()
{
	local name=$1
	local expected_state=$2
	local expected_error=$3
	local expected_recover=$4
	local expected_grace_period=$5
	local expected_auto_recover=$6

	local show=$(devlink health show $DL_HANDLE reporter $name -j | jq -e -r ".[][][]")
	check_err $? "Failed show $name reporter"

	local state=$(echo $show | jq -r ".state")
	[ "$state" == "$expected_state" ]
	check_err $? "Unexpected \"state\" value (got $state, expected $expected_state)"

	local error=$(echo $show | jq -r ".error")
	[ "$error" == "$expected_error" ]
	check_err $? "Unexpected \"error\" value (got $error, expected $expected_error)"

	local recover=`echo $show | jq -r ".recover"`
	[ "$recover" == "$expected_recover" ]
	check_err $? "Unexpected \"recover\" value (got $recover, expected $expected_recover)"

	local grace_period=$(echo $show | jq -r ".grace_period")
	check_err $? "Failed get $name reporter grace_period"
	[ "$grace_period" == "$expected_grace_period" ]
	check_err $? "Unexpected \"grace_period\" value (got $grace_period, expected $expected_grace_period)"

	local auto_recover=$(echo $show | jq -r ".auto_recover")
	[ "$auto_recover" == "$expected_auto_recover" ]
	check_err $? "Unexpected \"auto_recover\" value (got $auto_recover, expected $expected_auto_recover)"
}

dummy_reporter_test()
{
	RET=0

	check_reporter_info dummy healthy 0 0 0 true

	devlink health set $DL_HANDLE reporter dummy auto_recover false
	check_err $? "Failed to dummy reporter auto_recover option"

	check_reporter_info dummy healthy 0 0 0 false

	local BREAK_MSG="foo bar"
	echo "$BREAK_MSG"> $DEBUGFS_DIR/health/break_health
	check_err $? "Failed to break dummy reporter"

	check_reporter_info dummy error 1 0 0 false

	local dump=$(devlink health dump show $DL_HANDLE reporter dummy -j)
	check_err $? "Failed show dump of dummy reporter"

	local dump_break_msg=$(echo $dump | jq -r ".break_message")
	[ "$dump_break_msg" == "$BREAK_MSG" ]
	check_err $? "Unexpected dump break message value (got $dump_break_msg, expected $BREAK_MSG)"

	devlink health dump clear $DL_HANDLE reporter dummy
	check_err $? "Failed clear dump of dummy reporter"

	devlink health recover $DL_HANDLE reporter dummy
	check_err $? "Failed recover dummy reporter"

	check_reporter_info dummy healthy 1 1 0 false

	devlink health set $DL_HANDLE reporter dummy auto_recover true
	check_err $? "Failed to dummy reporter auto_recover option"

	check_reporter_info dummy healthy 1 1 0 true

	echo "$BREAK_MSG"> $DEBUGFS_DIR/health/break_health
	check_err $? "Failed to break dummy reporter"

	check_reporter_info dummy healthy 2 2 0 true

	local diagnose=$(devlink health diagnose $DL_HANDLE reporter dummy -j -p)
	check_err $? "Failed show diagnose of dummy reporter"

	local rcvrd_break_msg=$(echo $diagnose | jq -r ".recovered_break_message")
	[ "$rcvrd_break_msg" == "$BREAK_MSG" ]
	check_err $? "Unexpected recovered break message value (got $rcvrd_break_msg, expected $BREAK_MSG)"

	devlink health set $DL_HANDLE reporter dummy grace_period 10
	check_err $? "Failed to dummy reporter grace_period option"

	check_reporter_info dummy healthy 2 2 10 true

	echo "Y"> $DEBUGFS_DIR/health/fail_recover
	check_err $? "Failed set dummy reporter recovery to fail"

	echo "$BREAK_MSG"> $DEBUGFS_DIR/health/break_health
	check_fail $? "Unexpected success of dummy reporter break"

	check_reporter_info dummy error 3 2 10 true

	devlink health recover $DL_HANDLE reporter dummy
	check_fail $? "Unexpected success of dummy reporter recover"

	echo "N"> $DEBUGFS_DIR/health/fail_recover
	check_err $? "Failed set dummy reporter recovery to be successful"

	devlink health recover $DL_HANDLE reporter dummy
	check_err $? "Failed recover dummy reporter"

	check_reporter_info dummy healthy 3 3 10 true

	echo 8192 > $DEBUGFS_DIR/health/binary_len
	check_err $? "Failed set dummy reporter binary len to 8192"

	local dump=$(devlink health dump show $DL_HANDLE reporter dummy -j)
	check_err $? "Failed show dump of dummy reporter"

	devlink health dump clear $DL_HANDLE reporter dummy
	check_err $? "Failed clear dump of dummy reporter"

	log_test "dummy reporter test"
}

rate_leafs_get()
{
	local handle=$1

	cmd_jq "devlink port function rate show -j" \
	       '.[] | to_entries | .[] | select(.value.type == "leaf") | .key | select(contains("'$handle'"))'
}

rate_nodes_get()
{
	local handle=$1

	cmd_jq "devlink port function rate show -j" \
		'.[] | to_entries | .[] | select(.value.type == "node") | .key | select(contains("'$handle'"))'
}

rate_attr_set()
{
	local handle=$1
	local name=$2
	local value=$3
	local units=$4

	devlink port function rate set $handle $name $value$units
}

rate_attr_get()
{
	local handle=$1
	local name=$2

	cmd_jq "devlink port function rate show $handle -j" '.[][].'$name
}

rate_attr_tx_rate_check()
{
	local handle=$1
	local name=$2
	local rate=$3
	local debug_file=$4

	rate_attr_set $handle $name $rate mbit
	check_err $? "Failed to set $name value"

	local debug_value=$(cat $debug_file)
	check_err $? "Failed to read $name value from debugfs"
	[ "$debug_value" == "$rate" ]
	check_err $? "Unexpected $name debug value $debug_value != $rate"

	local api_value=$(( $(rate_attr_get $handle $name) * 8 / 1000000 ))
	check_err $? "Failed to get $name attr value"
	[ "$api_value" == "$rate" ]
	check_err $? "Unexpected $name attr value $api_value != $rate"
}

rate_attr_parent_check()
{
	local handle=$1
	local parent=$2
	local debug_file=$3

	rate_attr_set $handle parent $parent
	check_err $? "Failed to set parent"

	debug_value=$(cat $debug_file)
	check_err $? "Failed to get parent debugfs value"
	[ "$debug_value" == "$parent" ]
	check_err $? "Unexpected parent debug value $debug_value != $parent"

	api_value=$(rate_attr_get $r_obj parent)
	check_err $? "Failed to get parent attr value"
	[ "$api_value" == "$parent" ]
	check_err $? "Unexpected parent attr value $api_value != $parent"
}

rate_attr_tc_bw_check()
{
	local handle=$1
	local tc_bw=$2
	local debug_file=$3

	local tc_bw_str=""
	for bw in $tc_bw; do
		local tc=${bw%%:*}
		local value=${bw##*:}
		tc_bw_str="$tc_bw_str $tc:$value"
	done
	tc_bw_str=${tc_bw_str# }

	rate_attr_set "$handle" tc-bw "$tc_bw_str"
	check_err $? "Failed to set tc-bw values"

	for bw in $tc_bw; do
		local tc=${bw%%:*}
		local value=${bw##*:}
		local debug_value
		debug_value=$(cat "$debug_file"/tc"${tc}"_bw)
		check_err $? "Failed to read tc-bw value from debugfs for tc$tc"
		[ "$debug_value" == "$value" ]
		check_err $? "Unexpected tc-bw debug value for tc$tc: $debug_value != $value"
	done

	for bw in $tc_bw; do
		local tc=${bw%%:*}
		local expected_value=${bw##*:}
		local api_value
		api_value=$(rate_attr_get "$handle" tc_"$tc")
		if [ "$api_value" = "null" ]; then
			api_value=0
		fi
		[ "$api_value" == "$expected_value" ]
		check_err $? "Unexpected tc-bw value for tc$tc: $api_value != $expected_value"
	done
}

rate_node_add()
{
	local handle=$1

	devlink port function rate add $handle
}

rate_node_del()
{
	local handle=$1

	devlink port function rate del $handle
}

rate_test()
{
	RET=0

	echo $VF_COUNT > /sys/bus/netdevsim/devices/$DEV_NAME/sriov_numvfs
	devlink dev eswitch set $DL_HANDLE mode switchdev
	local leafs=`rate_leafs_get $DL_HANDLE`
	local num_leafs=`echo $leafs | wc -w`
	[ "$num_leafs" == "$VF_COUNT" ]
	check_err $? "Expected $VF_COUNT rate leafs but got $num_leafs"

	rate=10
	for r_obj in $leafs
	do
		rate_attr_tx_rate_check $r_obj tx_share $rate \
			$DEBUGFS_DIR/ports/${r_obj##*/}/tx_share
		rate=$(($rate+10))
	done

	rate=100
	for r_obj in $leafs
	do
		rate_attr_tx_rate_check $r_obj tx_max $rate \
			$DEBUGFS_DIR/ports/${r_obj##*/}/tx_max
		rate=$(($rate+100))
	done

	local tc_bw="0:0 1:40 2:0 3:0 4:0 5:0 6:60 7:0"
	for r_obj in $leafs
	do
		rate_attr_tc_bw_check "$r_obj" "$tc_bw" \
			"$DEBUGFS_DIR"/ports/"${r_obj##*/}"
	done

	local node1_name='group1'
	local node1="$DL_HANDLE/$node1_name"
	rate_node_add "$node1"
	check_err $? "Failed to add node $node1"

	local num_nodes=`rate_nodes_get $DL_HANDLE | wc -w`
	[ $num_nodes == 1 ]
	check_err $? "Expected 1 rate node in output but got $num_nodes"

	local node_tx_share=10
	rate_attr_tx_rate_check $node1 tx_share $node_tx_share \
		$DEBUGFS_DIR/rate_nodes/${node1##*/}/tx_share

	local node_tx_max=100
	rate_attr_tx_rate_check $node1 tx_max $node_tx_max \
		$DEBUGFS_DIR/rate_nodes/${node1##*/}/tx_max


	local tc_bw="0:20 1:0 2:0 3:0 4:0 5:20 6:60 7:0"
	rate_attr_tc_bw_check $node1 "$tc_bw" \
		"$DEBUGFS_DIR"/rate_nodes/"${node1##*/}"


	rate_node_del "$node1"
	check_err $? "Failed to delete node $node1"
	local num_nodes=`rate_nodes_get $DL_HANDLE | wc -w`
	[ $num_nodes == 0 ]
	check_err $? "Expected 0 rate node but got $num_nodes"

	local node1_name='group1'
	local node1="$DL_HANDLE/$node1_name"
	rate_node_add "$node1"
	check_err $? "Failed to add node $node1"

	rate_attr_parent_check $r_obj $node1_name \
		$DEBUGFS_DIR/ports/${r_obj##*/}/rate_parent

	local node2_name='group2'
	local node2="$DL_HANDLE/$node2_name"
	rate_node_add "$node2"
	check_err $? "Failed to add node $node2"

	rate_attr_parent_check $node2 $node1_name \
		$DEBUGFS_DIR/rate_nodes/$node2_name/rate_parent
	rate_node_del "$node2"
	check_err $? "Failed to delete node $node2"
	rate_attr_set "$r_obj" noparent
	check_err $? "Failed to unset $r_obj parent node"
	rate_node_del "$node1"
	check_err $? "Failed to delete node $node1"

	log_test "rate test"
}

setup_prepare()
{
	modprobe netdevsim
	echo "$BUS_ADDR $PORT_COUNT" > /sys/bus/netdevsim/new_device
	while [ ! -d $SYSFS_NET_DIR ] ; do :; done
}

cleanup()
{
	pre_cleanup
	echo "$BUS_ADDR" > /sys/bus/netdevsim/del_device
	modprobe -r netdevsim
}

trap cleanup EXIT

setup_prepare

tests_run

exit $EXIT_STATUS
