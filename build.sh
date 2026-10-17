usage()
{
	echo -e "Usage: $0 [clean]\n"
	exit 0
}

build_error()
{
	echo -e "\nError: Build process failed!\n"
	exit 1
}

clean_error()
{
	echo -e "\nError: Some cleaning failed!\n"
	exit 2
}


clean_failed=0

if [ "$1" = "-h" ] || [ "$1" = "help" ]
then
	usage
fi

if [ ! -z "$1" ] && [ ! "$1" = "clean" ]
then
	usage
fi

build_dir=$(dirname "$0")
target=$1

make -C "$build_dir" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/cartrom" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/ar_flash" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/tmdviewer" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/soundtest" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/synth" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/timviewer" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/cardtool" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

make -C "$build_dir/ps1-tools/linktest" $target
if [ $? -ne 0 ]; then
	if [ ! "$target" = "clean" ]; then
		build_error
	fi
	clean_failed=1
fi

if [ "$clean_failed" = "1" ]; then
	clean_error
fi
