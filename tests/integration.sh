#!/bin/sh
set -eu

whyrun=$1
test_file=$2
test_child=$3
test_network=$4
work=$5

mkdir -p "$work"
cd "$work"
printf 'WhyRun integration input\n' > input.txt

"$whyrun" record -- /bin/true > true.log
true_capsule=$(ls -1t run-*.wrun | head -1)
"$whyrun" show "$true_capsule" > true.show
grep -q '/bin/true' true.show

"$whyrun" record -- "$test_file" "$work/input.txt" "$work/output.txt" > file.log
file_capsule=$(ls -1t run-*.wrun | head -1)
"$whyrun" show "$file_capsule" --events > file.show
grep -q "$work/input.txt" file.show
grep -q "$work/output.txt" file.show
grep -q 'file_read' file.show
grep -q 'file_write' file.show

"$whyrun" record -- "$test_child" "$work/child.txt" > child.log
child_capsule=$(ls -1t run-*.wrun | head -1)
"$whyrun" show "$child_capsule" --events > child.show
grep -q "$work/child.txt" child.show
grep -q 'process_fork' child.show

"$whyrun" record -- "$test_network" > network.log
network_capsule=$(ls -1t run-*.wrun | head -1)
"$whyrun" show "$network_capsule" > network.show
grep -q '127.0.0.1:1' network.show
grep -q 'ECONNREFUSED' network.show

"$whyrun" diff "$true_capsule" "$file_capsule" > result.diff
grep -q '^+ WRITE ' result.diff
