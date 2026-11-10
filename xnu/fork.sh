# SPDX-License-Identifier: MIT-0
# xnu/fork.sh: sourced by xnu/apply.sh and xnu/apply-series.sh (HERE set). The kernel tree comes from Scion-Project/xnu, a fork
# of Apple's repository, where Scion's changes are ordinary commits on Apple's tags (docs/xnu-fork.md): the profile names the
# fork, its branch and the commit a build takes (profiles/*.conf: xnu_fork, fork_branch, fork_commit). What may not be in git
# (the Kernel Debug Kit's headers) is staged at build time by the profile's fork_stage scripts.
#   XNU_FORK_URL               where to fetch the fork from (default the profile's xnu_fork; a clone that has the commit will do)
# A profile that names no fork_commit does not build.
_scion_profile_get() { OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get "$1" 2>/dev/null; }
FORK_COMMIT=$(_scion_profile_get fork_commit || true)
[ -n "$FORK_COMMIT" ] || { echo "fork: the profile $(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" name) names no fork_commit: the kernel tree is a commit of Scion-Project/xnu (docs/xnu-fork.md)" >&2; exit 1; }
# fork_checkout DIR: DIR is the fork at the profile's commit, on the branch "hakios", and the current directory
fork_checkout() {
	local url=${XNU_FORK_URL:-$(_scion_profile_get xnu_fork)} branch=$(_scion_profile_get fork_branch || true) s
	git init -q "$1"
	cd "$1"
	git remote add origin "$url"
	git fetch -q --depth 1 origin "$FORK_COMMIT"
	git checkout -q -b hakios FETCH_HEAD
	[ "$(git rev-parse HEAD)" = "$FORK_COMMIT" ] || { echo "fork: $url gave $(git rev-parse HEAD), expected $FORK_COMMIT" >&2; return 1; }
	echo 'BUILD/' >> .git/info/exclude
	for s in $(_scion_profile_get fork_stage || true); do
		MACHINE_CONFIG=$(_scion_profile_get machine) MACOS_VERSION=${MACOS_VERSION:-26.5} bash "$HERE/../$s" ||
			{ echo "fork: $s failed" >&2; return 1; }
		git ls-files --others --exclude-standard | sed 's|^|/|' >> .git/info/exclude   # staged, not the fork's: kept out of git status
	done
	# this repository's files the tree takes (fork_copy: "SOURCE TARGET" lines): shared drivers that live here, not in the fork
	s=$(_scion_profile_get fork_copy || true)
	if [ -n "$s" ]; then
		grep -v '^#' "$HERE/../$s" | while read -r src dst; do
			[ -n "$src" ] || continue
			mkdir -p "$(dirname "$dst")" && cp -p "$HERE/../$src" "$dst" || { echo "fork: copy $src $dst failed" >&2; exit 1; }
			echo "/$dst" >> .git/info/exclude
		done || return 1
	fi
	echo "fork: $1: $url ${branch:-?} at ${FORK_COMMIT:0:12}"
}
