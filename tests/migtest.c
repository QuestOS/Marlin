/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vcpu.h>

int
main ()
{
  int i = 0;
  struct sched_param param;
  for (;;) {
    if (i == 5) {
      //printf ("Set sandbox machine to 1!\n");
      //param.machine_affinity = 1;
      param.affinity = 1;
      param.machine_affinity = -1;
      printf ("Set affinity to %d\n", param.affinity);
      sched_setparam (-1, &param);
    }
    printf ("Migration Test Process, counter = %d\n", i);
    usleep (1000000);
    i++;
  }
}

/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
