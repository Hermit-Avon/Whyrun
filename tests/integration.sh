#!/bin/sh
set -eu

whyrun=$1
test_file=$2
test_child=$3
test_network=$4
test_unix=$5
work_root=$6
work="$work_root/run-$$"

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

unix_path="$work/missing.sock"
"$whyrun" record -- "$test_unix" "$unix_path" > unix.log
unix_capsule=$(ls -1t run-*.wrun | head -1)
"$whyrun" show "$unix_capsule" --events > unix.show
grep -q 'Local IPC' unix.show
grep -q "unix:$unix_path" unix.show
grep -q 'local_ipc_connect' unix.show
grep -q 'connect -> ENOENT' unix.show
if grep -q '<sockaddr:family=1>' unix.show; then
    exit 1
fi
"$whyrun" diff "$true_capsule" "$unix_capsule" > unix.diff
grep -q "^+ unix:$unix_path" unix.diff

"$whyrun" diff "$true_capsule" "$file_capsule" > result.diff
grep -q '^+ WRITE ' result.diff

printf "cat '%s'\nfalse\necho \$?\nexit 0\n" "$work/input.txt" > session.input
"$whyrun" record < session.input > session.log 2> session.stderr
session_capsule=$(ls -1t run-*.wrun | head -1)
"$whyrun" show "$session_capsule" --events > session.show
grep -q '^Session$' session.show
grep -Fq "cat '$work/input.txt' (exit 0)" session.show
grep -Fq 'false (exit 1)' session.show
grep -Fq 'echo $? (exit 0)' session.show
grep -q "$work/input.txt" session.show
fork_count=$(grep -c ' process_fork ' session.show)
[ "$fork_count" -eq 1 ]
"$whyrun" diff "$true_capsule" "$session_capsule" > session.diff
grep -q '^Commands$' session.diff
grep -Fq "+ cat '$work/input.txt'" session.diff

visibility="$work/visibility"
mkdir -p "$visibility"
cd "$visibility"
"$whyrun" record -- /bin/sh -c "/bin/ls -la > '$work/observed-directory.txt'" \
    > "$work/visibility.log"
ls run-*.wrun > /dev/null
if grep -Eq '(run-.*\.wrun|\.wrun-journal|\.whyrun-)' "$work/observed-directory.txt"; then
    exit 1
fi
