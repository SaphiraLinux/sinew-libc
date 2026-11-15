#include <ftw.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

struct history
{
	struct history *chain;
	dev_t dev;
	ino_t ino;
	int level;
	int base;
};

#undef dirfd
#define dirfd(d) (*(int *)d)

static int do_nftw(char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *), int fd_limit, int flags, struct history *h, int dl)
{
	size_t l = strlen(path), j = l && path[l-1]=='/' ? l-1 : l;
	struct stat st;
	struct history new;
	int type;
	int r;
	int dfd;
	int err;
	struct FTW lev;

	st.st_dev = st.st_ino = 0;

	if ((flags & FTW_PHYS) ? lstat(path, &st) : stat(path, &st) < 0) {
		if (!(flags & FTW_PHYS) && errno==ENOENT && !lstat(path, &st))
			type = FTW_SLN;
		else if (errno != EACCES) return -1;
		else type = FTW_NS;
	} else if (S_ISDIR(st.st_mode)) {
		if (flags & FTW_DEPTH) type = FTW_DP;
		else type = FTW_D;
	} else if (S_ISLNK(st.st_mode)) {
		if (flags & FTW_PHYS) type = FTW_SL;
		else type = FTW_SLN;
	} else {
		type = FTW_F;
	}

	if ((flags & FTW_MOUNT) && h && type != FTW_NS && st.st_dev != h->dev)
		return 0;
	
	new.chain = h;
	new.dev = st.st_dev;
	new.ino = st.st_ino;
	new.level = h ? h->level+1 : 0;
	/* Base indexes the caller-visible spelling (path+dl); with no
	 * CHDIR absolutization dl is 0 and nothing changes. */
	new.base = j+1-dl;
	
	lev.level = new.level;
	if (h) {
		lev.base = h->base;
	} else {
		size_t k;
		for (k=j; k && path[k]=='/'; k--);
		for (; k && path[k-1]!='/'; k--);
		lev.base = k-dl;
	}

	if (type == FTW_D || type == FTW_DP) {
		dfd = open(path, O_RDONLY);
		err = errno;
		if (dfd < 0 && err == EACCES) type = FTW_DNR;
		if (!fd_limit) close(dfd);
	}

	if (!(flags & FTW_DEPTH) && (r=fn(path+dl, &st, type, &lev)))
		return r;

	for (; h; h = h->chain)
		if (h->dev == st.st_dev && h->ino == st.st_ino)
			return 0;

	if ((type == FTW_D || type == FTW_DP) && fd_limit) {
		if (dfd < 0) {
			errno = err;
			return -1;
		}
		DIR *d = fdopendir(dfd);
		if (d) {
			struct dirent *de;
			if (flags & FTW_CHDIR) {
				/* Descend: subsequent names are resolved
				 * from inside this directory. */
				if (fchdir(dfd) < 0) {
					err = errno;
					closedir(d);
					errno = err;
					return -1;
				}
			}
			while ((de = readdir(d))) {
				if (de->d_name[0] == '.'
				 && (!de->d_name[1]
				  || (de->d_name[1]=='.'
				   && !de->d_name[2]))) continue;
				if (strlen(de->d_name) >= PATH_MAX-l) {
					errno = ENAMETOOLONG;
					closedir(d);
					return -1;
				}
				path[j]='/';
				strcpy(path+j+1, de->d_name);
				r = do_nftw(path, fn, fd_limit-1, flags, &new, dl);
				if (flags & FTW_CHDIR) {
					/* The child left the working directory
					 * somewhere below us; restore it so the
					 * next sibling (and fn) sees this dir. */
					int e2 = errno;
					if (fchdir(dfd) < 0) {
						err = errno;
						closedir(d);
						errno = err;
						return -1;
					}
					errno = e2;
				}
				if (r) {
					closedir(d);
					return r;
				}
			}
			closedir(d);
		} else {
			close(dfd);
			return -1;
		}
	}

	path[l] = 0;
	/* Post-order visits run with the working directory still inside
	 * this directory (each child restored it on return), matching the
	 * observable glibc behavior; the wrapper restores the caller. */
	if ((flags & FTW_DEPTH) && (r=fn(path+dl, &st, type, &lev)))
		return r;

	return 0;
}

int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *), int fd_limit, int flags)
{
	int r, cs;
	size_t l;
	char pathbuf[PATH_MAX+1];
	int orig_dfd = -1;
	/* Offset of the caller-visible spelling within pathbuf. Zero
	 * unless CHDIR anchored a relative root. */
	int dl = 0;

	if (fd_limit <= 0) return 0;

	l = strlen(path);
	if (l > PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (flags & FTW_CHDIR) {
		if (path[0] != '/') {
			/* Descent builds on the root string after each
			 * fchdir, so a relative root would stop resolving.
			 * Anchor it to the current directory up front. */
			char cwdbuf[PATH_MAX+1];
			size_t cl;
			if (!getcwd(cwdbuf, sizeof cwdbuf))
				return -1;
			cl = strlen(cwdbuf);
			if (cl + 1 + l > PATH_MAX) {
				errno = ENAMETOOLONG;
				return -1;
			}
			memcpy(pathbuf, cwdbuf, cl);
			pathbuf[cl] = '/';
			memcpy(pathbuf+cl+1, path, l+1);
			l = cl + 1 + l;
			dl = cl + 1;
		} else {
			memcpy(pathbuf, path, l+1);
		}
		/* Save the caller's directory; restored below on every
		 * path out, success or failure (recovery is best-effort
		 * only if this open itself fails). */
		orig_dfd = open(".", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
		if (orig_dfd < 0)
			return -1;
	} else {
		memcpy(pathbuf, path, l+1);
	}

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	r = do_nftw(pathbuf, fn, fd_limit, flags, NULL, dl);
	if (flags & FTW_CHDIR) {
		/* Restore before re-enabling cancellation so a pending
		 * cancel cannot strand the caller elsewhere. */
		int e = errno;
		if (fchdir(orig_dfd) < 0) {
			if (r == 0) r = -1;
			else errno = e;
		} else {
			errno = e;
		}
	}
	pthread_setcancelstate(cs, 0);
	if (flags & FTW_CHDIR) {
		int e = errno;
		close(orig_dfd);
		errno = e;
	}
	return r;
}
