if [[ -z "$run_id" ]]; then
	export run_id=$(uuidgen)
fi

if [[ "$GITHUB_ACTIONS" == "true" ]]; then
	export _n='%0A'
	export _c='%2C'
else
	export _n=$'\n'
	export _c=$','
	export _color='--color'
fi

[[ ! -v CI_WORKTREE ]] && export CI_WORKTREE='.'

_fmt() {
	msg=$(echo "$1" | tr -d '\t')
	if [[ ! "$_n" == $'\n' ]]; then
		msg=$(printf "$msg" | sed ':a;N;$!ba;s/\n/'"$_n"'/g')
	fi
	printf "%s\n" "$msg"
}

_file () {
	echo "$1" | sed 's/,/'"$_c"'/g'
}

check_checkpatch() {
	export step_name="checkpatch"
	local strategy=${1:-commit} # or per file
	local mail=
	local fail=0
	local warn=0
	local ignored=
	local tmp_branch_name=$(git symbolic-ref --short HEAD)-$RANDOM
	local checkpatch_opts

	checkpatch_opts="--strict \
			--ignore FILE_PATH_CHANGES \
			--ignore LONG_LINE \
			--ignore LONG_LINE_STRING \
			--ignore LONG_LINE_COMMENT \
			--ignore PARENTHESIS_ALIGNMENT \
			--ignore CAMELCASE \
			--ignore UNDOCUMENTED_DT_STRING \
			--ignore UNKNOWN_COMMIT_ID \
		"

	echo "$step_name on range $base_sha..$head_sha"

	for ignore in $CHECKPATCH_EXTRAIGNORES; do
		checkpatch_opts="$checkpatch_opts --ignore $ignore"
	done

	if [[ "$strategy" == "file" ]]; then
		# To still only check the changes, squash the range in a single commit
		git switch -c $tmp_branch_name 1>/dev/null
		git reset --soft $base_sha
		git commit -m "[TEMP] check: Squashed range" -m "Signed-off-by: CSE CI <cse-ci-notifications@analog.com>"

		checkpatch_opts="$checkpatch_opts \
			--ignore COMMIT_MESSAGE \
			--ignore DT_SPLIT_BINDING_PATCH"

		[[ "$GITHUB_ACTIONS" != "true" ]] && echo "Attention! At temp branch '$tmp_branch_name'. If you cancel (Ctrl+C), also revert with 'git switch -'"
	fi
	[[ "$strategy" == "commit" ]] && commits=$(git rev-list $base_sha..$head_sha --reverse) || commits=$(git rev-parse @)

	python3 -m venv ~/venv
	source ~/venv/bin/activate
	python3 -m ensurepip --upgrade
	pip3 install ply GitPython --upgrade

	# The output is not properly captured with --git
	for commit in $commits; do
		git --no-pager log --oneline $commit~..$commit
		# Skip empty commits, assume cover letter
		# and those only touching non-upstream directories .github ci and docs
		# A treeless (--filter=tree:0) fetch could be done to fetch
		# full commit message history before running checkpatch, but
		# that may mess-up the cache and is only useful for checking
		# SHA references in the commit message, with may not even apply
		# if the commit is from upstream. Instead, just delegate to the
		# user to double check the referenced SHA.
		local files=$(git diff --diff-filter=ACM --no-renames --name-only "$commit~..$commit" | grep -v ^ci | grep -v ^.github | grep -v ^docs || true)
		if [[ -z "$files" ]]; then
			echo "empty, skipped"
			continue
		else
			echo $files
		fi

		_echo_ci "scripts/checkpatch.pl --git $commit $checkpatch_opts"
		mail=$(scripts/checkpatch.pl \
			--git "$commit" \
			$checkpatch_opts )

		ignored=0
		found=0
		msg=

		while read -r row
		do
			if [[ "$row" =~ ^total: ]]; then
				echo -e "\e[1m$row\e[0m"
				break
			fi

			# Additional parsing is needed
			if [[ "$found" == "1" ]]; then

				# The row is started with "#"
				if [[ "$row" =~ ^\# ]]; then
					# Split the string using ':' separator
					IFS=':' read -r -a list <<< "$row"

					# Get file-name after removing spaces.
					file=$(echo ${list[2]} | xargs)

					# Get line-number
					line=${list[3]}
					_echo_ci $row
				else
					msg=${msg}${row}$_n
					if [[ -z $row ]]; then
						if [[ -z $file ]]; then
							# If no file, add to file 0 of first file on list.
							file=$(git show --name-only --pretty=format: $commit | head -n 1)
							echo "::$type file=$(_file "$file"),line=0::$step_name: $msg"
						else
							[[ "$type" == "error" ]] && fail=1
							[[ "$type" == "warning" ]] && warn=1
							echo "::$type file=$(_file "$file"),line=$line::$step_name: $msg"
						fi
						found=0
						file=
						line=
						type=
					fi
				fi
			fi

			if [[ "$row" =~ ^ERROR: ]]; then
				type="error"
			elif [[ "$row" =~ ^WARNING: ]]; then
				type="warning"
			elif [[ "$row" =~ ^CHECK: ]]; then
				type="warning"
			fi
			if [[ "$row" =~ ^(CHECK|WARNING|ERROR): ]]; then
				msg=$(echo "$row" | sed -E  's/^(CHECK|WARNING|ERROR): //')$_n
				found=1
				file=
				line=
			else
				if [[ "$found" == "0" ]]; then
					echo $row
				fi
			fi

		done <<< "$mail"
		echo "       of those, $ignored were ignored"
	done

	[[ "$strategy" == "file" ]] && (git switch - 1>/dev/null; git branch -D $tmp_branch_name)

	_set_step_warn $warn
	[[ "$fail" == "1" ]] && set_step_fail "$step_name"
	return $fail
}

check_coccicheck() {
	export step_name="coccicheck"
	local files=$(git diff --diff-filter=ACM --no-renames --name-only $base_sha..$head_sha -- '**/*.c')
	local mail=
	local warn=0

	echo "$step_name on range $base_sha..$head_sha"

	[[ -z "$ARCH" ]] && ARCH=x86

	coccis=$(ls scripts/coccinelle/**/*.cocci)
	[[ -z "$coccis" ]] && return 0
	[[ -z "$files" ]] && return 0
	while read file; do
		echo -e "\e[1m$file\e[0m"

		while read cocci; do

			mail=$(spatch -D report  --very-quiet --cocci-file $cocci --no-includes \
				    --include-headers --patch . \
				    -I arch/$ARCH/include -I arch/$ARCH/include/generated -I include \
				    -I arch/$ARCH/include/uapi -I arch/$ARCH/include/generated/uapi \
				    -I include/uapi -I include/generated/uapi \
				    --include include/linux/compiler-version.h \
				    --include include/linux/kconfig.h $file || true)

			msg=

			while read -r row
			do
				# There is no standard for cocci, so this is the best we can do
				# is match common beginnings and log the rest
				if [[ "$row" =~ ^$file: ]]; then
					type="warning"
					warn=1
				fi

				if [[ "$row" =~ ^(warning): ]]; then
					# warning: line 223: should nonseekable_open be a metavariable?
					# internal cocci warning, not user fault
					echo $row
				elif [[ "$row" =~ ^$file: ]]; then
					# drivers/iio/.../adi_adrv9001_fh.c:645:67-70: duplicated argument to & or |
					IFS=':' read -r -a list <<< "$row"
					line=${list[1]}
					msg=
					for ((i=2; i<${#list[@]}; i++)); do
						msg="$msg${list[$i]} "
					done
					echo "::$type file=$(_file "$file"),line=$line::$step_name: $msg"
				else
					if [[ "$row" ]]; then
						echo $row
					fi
				fi

			done <<< "$mail"

		done <<< "$coccis"

	done <<< "$files"

	_set_step_warn $warn
	return 0
}

_bad_licence_error() {
	local license_error="
	File is being added to Analog Devices U-Boot tree.
	Analog Devices code is being marked dual-licensed... Make sure this is really intended!
	If not intended, change MODULE_LICENSE() or the SPDX-License-Identifier accordingly.
	This is not as simple as one thinks and upstream might require a lawyer to sign the patches!"
	_fmt "::warning file=$1::$step_name: $license_error"
}

check_license() {
	export step_name="check_license"
	local fail=0

	echo "$step_name on range $base_sha..$head_sha"

	local added_files=$(git diff --diff-filter=A --name-only "$base_sha..$head_sha")

	# Get list of new files in the commit range
	for file in $added_files ; do
		if git diff $base_sha..$head_sha "$file" 2>/dev/null | grep "^+MODULE_LICENSE" | grep -q "Dual" ; then
			_bad_licence_error "$file"
			fail=1
		elif git diff $base_sha..$head_sha "$file" 2>/dev/null | grep "^+// SPDX-License-Identifier:" | grep -qi " OR " ; then
			# The below might catch bad licenses in header files and also helps to make sure dual licenses are
			# not in driver (e.g.: sometimes people have MODULE_LICENSE != SPDX-License-Identifier - which is also
			# wrong and maybe something to improve in this job).
			# For devicetree-related files, allow dual license if GPL-2.0 is one of them.
			if [[ "$file" == *.@(yaml|dts|dtsi|dtso) ]]; then
				if cat "$file" | grep "^// SPDX-License-Identifier:" | grep -q "GPL-2.0" ; then
					continue
				fi
			fi
			_bad_licence_error "$file"
			fail=1
		fi
	done

	_set_step_warn $warn
	[[ "$fail" == "1" ]] && set_step_fail "$step_name"
	return $fail
}

check_qconfig_sync() {
	export step_name="check_qconfig_sync"
	local fail=0
	local list=$(mktemp)
	local out
	local changed=0

	echo "$step_name"

	printf '%s\n' "$@" > "$list"
	git diff --diff-filter=A --name-only "$base_sha..$head_sha" \
		-- 'arch/*/configs/*_defconfig' | xargs -r -n1 basename >> "$list"

	if [[ ! -s "$list" ]]; then
		rm -f "$list"
		return 0
	fi

	local arm_gcc aarch64_gcc
	arm_gcc=$(command -v arm-none-eabi-gcc 2>/dev/null || true)
	aarch64_gcc=$(command -v aarch64-linux-gnu-gcc 2>/dev/null || true)
	if [[ -n "$arm_gcc" || -n "$aarch64_gcc" ]]; then
		cat > ~/.buildman <<-EOF
		[toolchain-prefix]
		arm = ${arm_gcc%gcc}
		aarch64 = ${aarch64_gcc%gcc}
		EOF
	fi

	python3 tools/qconfig.py -s -d "$list"

	out=$(git diff $_color HEAD --name-only)
	if [[ -n "$out" ]]; then
		git diff $_color HEAD
		while read -r file; do
			[[ -z "$file" ]] && continue
			_fmt "::error file=$(_file "$file")::$step_name: qconfig.py modified this file."
			changed=1
		done <<< "$out"
		fail=1
	fi

	out=$(git ls-files --others --exclude-standard -- ':!ci/' ':!qconfig.db' ':!qconfig.failed')
	if [[ -n "$out" ]]; then
		while read -r file; do
			[[ -z "$file" ]] && continue
			echo "$file"
			_fmt "::error file=$(_file "$file")::$step_name: qconfig.py created this untracked file."
			changed=1
		done <<< "$out"
		[[ "$changed" == "1" ]] && fail=1
	fi

	[[ "$changed" == "0" ]] && echo "qconfig.py produced no tree changes"

	git restore .
	git clean -fd -e ci/
	rm -f "$list"

	[[ "$fail" == "1" ]] && set_step_fail "$step_name"
	return $fail
}

apply_prerun() {
	export step_name="apply_prerun"
	# Run cocci and bash scripts from ci/prerun
	# e.g. manipulate the source code depending on run conditons or target.
	local coccis=$(ls ci/prerun/*.cocci 2>/dev/null)
	local bashes=$(ls ci/prerun/*.sh 2>/dev/null)
	local files=$(git diff --diff-filter=ACM --no-renames --name-only $base_sha..$head_sha -- '**/*.c')

	echo "$step_name on range $base_sha..$head_sha"
	[[ -z "$files" ]] && return 0
	if [[ ! -z "$coccis" ]]; then
		while read cocci; do
			echo "apply $cocci"
			while read file; do
				echo -e "\e[1m$cocci $file\e[0m"
				spatch --sp-file $cocci --in-place $file
			done <<< "$files"
		done <<< "$coccis"
	fi

	if [[ ! -z "$bashes" ]]; then
		while read bashs; do
			echo "apply $bashs"
			while read file; do
				echo -e "\e[1m$bashs $file\e[0m"
				$bashs $file
			done <<< "$files"
		done <<< "$bashes"
	fi
}

set_step_warn () {
	[[ -z "$GITHUB_ENV" ]] && return
	echo ; echo "step_warn_$1=true" >> "$GITHUB_ENV"
}

set_step_fail () {
	[[ -z "$GITHUB_ENV" ]] && return
	echo ; echo "step_fail_$1=true" >> "$GITHUB_ENV"
}

_set_step_warn () {
	if [[ "$1" == "1" ]] || [[ "$1" == "true" ]] ; then
		set_step_warn "$step_name"
	fi
}

_set_step_fail () {
	if [[ ! -z "$step_name" ]]; then
		set_step_fail "$step_name"
		unset step_name
	fi
}

_echo_ci () {
	[[ "$GITHUB_ACTIONS" != "true" ]] && return
	echo "$@"
}

trap '_set_step_fail' ERR
