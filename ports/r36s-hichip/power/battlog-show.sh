# Read-only: the battery log so far (first lines, last lines, line count).
test -f /tmp/battlog.txt || { echo NO_BATTLOG; exit 1; }
echo "LINES $(grep -c . /tmp/battlog.txt)"
head -n 3 /tmp/battlog.txt
echo ...
tail -n 15 /tmp/battlog.txt
