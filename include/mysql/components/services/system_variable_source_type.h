/*
   Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SYSTEM_VARIABLE_SOURCE_TYPE_H
#define SYSTEM_VARIABLE_SOURCE_TYPE_H
/**
  This enum values define how system variables are set. For example if a
  variable is set by global option file /etc/my.cnf then source will be
  set to GLOBAL, or if a variable is set from command line then source
  will hold value as COMMAND_LINE.
*/
enum enum_variable_source
{
  COMPILED= 1,
  GLOBAL,
  SERVER,
  EXPLICIT,
  EXTRA,
  MYSQL_USER,
  LOGIN,
  COMMAND_LINE,
  PERSISTED,
  DYNAMIC
};

#endif /* SYSTEM_VARIABLE_SOURCE_TYPE_H */
