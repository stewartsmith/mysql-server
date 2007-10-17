/* Copyright (C) 2006 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* get the number of (online) CPUs */

#include "mysys_priv.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

static int ncpus=0;

int my_getncpus()
{
  if (!ncpus)
  {
#ifdef _SC_NPROCESSORS_ONLN
    ncpus= sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(__WIN__)
#ifdef REGGIE_WILL_FIX_IN_A_PORTABLE_WAY
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    ncpus= sysinfo.dwNumberOfProcessors;
#else
    ncpus=2;
#endif
#else
/* unknown so play safe: assume SMP and forbid uniprocessor build */
    ncpus= 2;
#endif
  }
  return ncpus;
}
