#!/bin/sh

echo "This is a dummy autogenerated file to make automake happy; please ignore it." > ChangeLog &&
touch po/POTFILES.in &&
aclocal-1.9 -I m4 &&
autoheader &&
automake-1.9 --add-missing &&
aclocal-1.9 -I m4 &&
autoconf &&
autoheader
