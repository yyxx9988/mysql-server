/*****************************************************************************

Copyright (c) 2017 Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/clone0api.h
Innodb Clone Interface

*******************************************************/

#ifndef CLONE_API_INCLUDE
#define CLONE_API_INCLUDE

#include "univ.i"
#include "handler.h"

/** Begin copy from source database
@param[in]	hton	handlerton for SE
@param[in]	thd	server thread handle
@param[in,out]	loc	locator
@param[in,out]	loc_len	locator length
@param[in]	type	clone type
@return error code */
int innodb_clone_begin(
	handlerton*	hton,
	THD*		thd,
	byte*&		loc,
	uint&		loc_len,
	Ha_clone_type	type);

/** Copy data from source database in chunks via callback
@param[in]	hton	handlerton for SE
@param[in]	thd	server thread handle
@param[in]	loc	locator
@param[in]	cbk	callback interface for sending data
@return error code */
int innodb_clone_copy(
	handlerton*	hton,
	THD*		thd,
	byte*		loc,
	Ha_clone_cbk*	cbk);

/** End copy from source database
@param[in]	hton	handlerton for SE
@param[in]	thd	server thread handle
@param[in]	loc	locator
@return error code */
int innodb_clone_end(
	handlerton*	hton,
	THD*		thd,
	byte*		loc);

/** Begin apply to destination database
@param[in]	hton		handlerton for SE
@param[in]	thd		server thread handle
@param[in,out]	loc		locator
@param[in,out]	loc_len		locator length
@param[in]	data_dir	target data directory
@return error code */
int innodb_clone_apply_begin(
	handlerton*	hton,
	THD*		thd,
	byte*&		loc,
	uint&		loc_len,
	const char*	data_dir);

/** Apply data to destination database in chunks via callback
@param[in]	hton	handlerton for SE
@param[in]	thd	server thread handle
@param[in]	loc	locator
@param[in]	cbk	callback interface for receiving data
@return error code */
int innodb_clone_apply(
	handlerton*	hton,
	THD*		thd,
	byte*		loc,
	Ha_clone_cbk*	cbk);

/** End apply to destination database
@param[in]	hton	handlerton for SE
@param[in]	thd	server thread handle
@param[in]	loc	locator
@return error code */
int innodb_clone_apply_end(
	handlerton*	hton,
	THD*		thd,
	byte*		loc);

/** Initialize Clone system */
void clone_init();

/** Uninitialize Clone system */
void clone_free();

/** Mark clone system for abort to disallow database clone
@param[in]	force	abort running database clones
@return true if successful. */
bool clone_mark_abort(bool force);

/** Mark clone system as active to allow database clone. */
void clone_mark_active();

#endif /* CLONE_API_INCLUDE */
