/*
  vector_3.cpp - Vector library for bed leveling
  Copyright (c) 2012 Lars Brubaker.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <math.h>
#include "Marlin.h"

#ifdef ENABLE_AUTO_BED_LEVELING
#include "vector_3.h"

void vector_3_init_3(vector_3 *v, float x, float y, float z)
{
  v->x = x;
  v->y = y;
  v->z = z;
}

void vector_3_cross(vector_3 *l, vector_3 *r, vector_3 *result)
{
  result->x = l->y * r->z - l->z * r->y;
  result->y = l->z * r->x - l->x * r->z;
  result->z = l->x * r->y - l->y * r->x;
}

void vector_3_sub(vector_3 *l, vector_3 *r, vector_3 *result)
{
  result->x = l->x - r->x;
  result->y = l->y - r->y;
  result->z = l->z - r->z;
}

void vector_3_get_normal(vector_3 *v)
{
  vector_3_normalize(v);
}

float vector_3_get_length(vector_3 *v)
{
  float length = sqrt((v->x * v->x) + (v->y * v->y) 
                      + (v->z * v->z));
  return length;
}

void vector_3_normalize(vector_3 *v)
{
  float length = vector_3_get_length(v);
  v->x /= length;
  v->y /= length;
  v->z /= length;
}

void vector_3_apply_rotation(vector_3 *v, matrix_3x3 matrix)
{
  float resultX = v->x * matrix.matrix[3*0+0] + v->y * matrix.matrix[3*1+0] + v->z * matrix.matrix[3*2+0];
  float resultY = v->x * matrix.matrix[3*0+1] + v->y * matrix.matrix[3*1+1] + v->z * matrix.matrix[3*2+1];
  float resultZ = v->x * matrix.matrix[3*0+2] + v->y * matrix.matrix[3*1+2] + v->z * matrix.matrix[3*2+2];
  
  v->x = resultX;
  v->y = resultY;
  v->z = resultZ;
}

void matrix_3x3_set_to_identity(matrix_3x3 *m)
{
  m->matrix[0] = 1; m->matrix[1] = 0; m->matrix[2] = 0;
  m->matrix[3] = 0; m->matrix[4] = 1; m->matrix[5] = 0;
  m->matrix[6] = 0; m->matrix[7] = 0; m->matrix[8] = 1;
}

void matrix_3x3_create_from_rows(matrix_3x3 *new_matrix, vector_3 row_0, 
                                vector_3 row_1, vector_3 row_2)
{
  new_matrix->matrix[0] = row_0.x; new_matrix->matrix[1] = row_0.y; new_matrix->matrix[2] = row_0.z;
  new_matrix->matrix[3] = row_1.x; new_matrix->matrix[4] = row_1.y; new_matrix->matrix[5] = row_1.z;
  new_matrix->matrix[6] = row_2.x; new_matrix->matrix[7] = row_2.y; new_matrix->matrix[8] = row_2.z;
}

matrix_3x3 matrix_3x3_create_look_at(vector_3 target)
{
  vector_3 z_row, x_row, y_row;
  z_row = target;
  vector_3_get_normal(&z_row);
  vector_3_init_3(&x_row, 1, 0, -target.x/target.z);
  vector_3_get_normal(&x_row);
  vector_3_init_3(&y_row, 0, 1, -target.y/target.z);
  vector_3_get_normal(&y_row);

  // create the matrix already correctly transposeda
  matrix_3x3 rot;
  matrix_3x3_create_from_rows(&rot, x_row, y_row, z_row);

  return rot;
}

void apply_rotation_xyz(matrix_3x3 matrix, float *x, float *y, float *z)
{
  vector_3 vector;
  vector_3_init_3(&vector, *x, *y, *z);
  vector_3_apply_rotation(&vector, matrix);
  *x = vector.x;
  *y = vector.y;
  *z = vector.z;
}

matrix_3x3 matrix_3x3_transpose(matrix_3x3 original)
{
  matrix_3x3 new_matrix;
  new_matrix.matrix[0] = original.matrix[0]; new_matrix.matrix[1] = original.matrix[3]; new_matrix.matrix[2] = original.matrix[6]; 
  new_matrix.matrix[3] = original.matrix[1]; new_matrix.matrix[4] = original.matrix[4]; new_matrix.matrix[5] = original.matrix[7]; 
  new_matrix.matrix[6] = original.matrix[2]; new_matrix.matrix[7] = original.matrix[5]; new_matrix.matrix[8] = original.matrix[8];
  return new_matrix;
}


#endif // #ifdef ENABLE_AUTO_BED_LEVELING

/* vi: set et sw=2 sts=2: */
