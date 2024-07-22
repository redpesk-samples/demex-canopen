#!/bin/sh

ifile=${1:-/dev/stdin}
sname=${2:-infotxt}

echo "/* generated from ${ifile} */"
echo -n "const char ${sname}[] = "
sed 's/"/\\"/g;s/^\([ \t]*\)\(.*\)\([ \t]*\)$/\1"\2"\3/' "${ifile}"
echo ';'
