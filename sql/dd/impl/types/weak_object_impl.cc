/* Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "sql/dd/impl/types/weak_object_impl.h"

#include <memory>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_loglevel.h"
#include "my_sys.h"
#include "mysqld_error.h"                 // ER_*
#include "sql/dd/impl/object_key.h"       // Needed for destructor
#include "sql/dd/impl/raw/raw_record.h"   // Raw_record
#include "sql/dd/impl/raw/raw_table.h"    // Raw_table
#include "sql/dd/impl/transaction_impl.h" // Open_dictionary_tables_ctx
#include "sql/dd/impl/types/entity_object_impl.h"
#include "sql/dd/string_type.h"
#include "sql/dd/types/object_table.h"    // Object_table
#include "sql/log.h"

namespace dd {

///////////////////////////////////////////////////////////////////////////

/**
  @brief
  Store the DD object into DD table.

  @param otx - DD transaction in use.

  @return true - on failure and error is reported.
  @return false - on success.
*/
bool Weak_object_impl::store(Open_dictionary_tables_ctx *otx)
{
  DBUG_ENTER("Weak_object_impl::store");

  DBUG_EXECUTE_IF("fail_while_storing_dd_object",
  {
    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
    DBUG_RETURN(true);
  });

  const Object_table &obj_table= this->object_table();

  // Get main object table.

  Raw_table *t= otx->get_table(obj_table.name());

  DBUG_ASSERT(t);

  // Insert or update record.

  do
  {
    /*
      If we know that object has new primary key (e.g. to be generated
      at insert time) we can skip looking up and updating old record.
      This measure greatly reduces probability of InnoDB deadlocks between
      concurrent DDL. Deadlocks occur because each of concurrent DDL
      first looks up record with non-existing PK (e.g. INVALID_OBJECT_ID
      or value greater than existing PK values for non-Entity objects) and
      this acquires gap lock on supremum record and then tries to insert
      row into this gap.
    */
    if (this->has_new_primary_key())
      break;

    std::unique_ptr<Object_key> obj_key(this->create_primary_key());

    if (!obj_key.get())
    {
      /* purecov: begin deadcode */
      LogErr(ERROR_LEVEL, ER_DD_CANT_GET_OBJECT_KEY);
      DBUG_ASSERT(false);
      DBUG_RETURN(true);
      /* purecov: end */
    }

    std::unique_ptr<Raw_record> r;
    if (t->prepare_record_for_update(*obj_key, r))
      DBUG_RETURN(true);

    if (!r.get())
      break;

    // Existing record found -- do an UPDATE.

    if (this->store_attributes(r.get()))
    {
      my_error(ER_UPDATING_DD_TABLE,
               MYF(0),
               obj_table.name().c_str());
      DBUG_RETURN(true);
    }

    if (r->update())
      DBUG_RETURN(true);

    DBUG_RETURN(store_children(otx));
  }
  while (false);

  // No existing record exists -- do an INSERT.

  std::unique_ptr<Raw_new_record> r(t->prepare_record_for_insert());

  // Store attributes.

  if (this->store_attributes(r.get()))
  {
    my_error(ER_UPDATING_DD_TABLE,
             MYF(0),
             obj_table.name().c_str());
    DBUG_RETURN(true);
  }

  if (r->insert())
    DBUG_RETURN(true);

  DBUG_EXECUTE_IF("weak_object_impl_store_fail_before_store_children",
                  {
                    my_error(ER_UNKNOWN_ERROR, MYF(0));
                    DBUG_RETURN(true);
                  });

  this->set_primary_key_value(*r);

  /*
    It is necessary to destroy the Raw_new_record() object after
    inserting the parent DD object and before creating children
    DD object. The reason is that, the parent DD object may
    insert a row in the same DD table (E.g., when storing parent
    partition metadata in mysql.partitions), in which even the
    child DD table would end-up inserting a row. (e.g., where
    storing sub partition metadata in mysql.partitions)
    Destroying Raw_new_record() enable accurate computation of
    the auto increment values, for the child partition entry
    being stored.
  */
  r.reset();

  if (store_children(otx))
    DBUG_RETURN(true);

  /*
    Mark object as having existing PK only after processing its children.
    This allows non-entity children to rely on parent has_new_primary_key()
    method to figure out if their primary key based on parent's one was not
    used before.
  */
  this->fix_has_new_primary_key();

  DBUG_RETURN(false);
}

///////////////////////////////////////////////////////////////////////////

/**
  @brief
  Drop the DD object from DD table.

  @param otx - DD transaction in use.

  @return true - on failure and error is reported.
  @return false - on success.
*/
bool Weak_object_impl::drop(Open_dictionary_tables_ctx *otx) const
{
  DBUG_ENTER("Weak_object_impl::drop");

  DBUG_EXECUTE_IF("fail_while_dropping_dd_object",
                  {
                    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
                    DBUG_RETURN(true);
                  });


  const Object_table &obj_table= this->object_table();

  // Get main object table.

  Raw_table *t= otx->get_table(obj_table.name());

  DBUG_ASSERT(t);

  // Find object to be dropped

  std::unique_ptr<Object_key> obj_key(this->create_primary_key());

  std::unique_ptr<Raw_record> r;
  if (t->prepare_record_for_update(*obj_key, r))
    DBUG_RETURN(true);

  if (!r.get())
  {
    /* purecov: begin deadcode */
    LogErr(ERROR_LEVEL, ER_DD_CANT_CREATE_OBJECT_KEY);
    DBUG_ASSERT(false);
    DBUG_RETURN(true);
    /* purecov: end */
  }

  /**
    Drop collections and then drop the object

    We should drop collections first and then parent object
    as we have referencial constraints. Mostly the reverse
    order of restore/store operation.
  */

  if (this->drop_children(otx) || r->drop())
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}

///////////////////////////////////////////////////////////////////////////

bool Weak_object_impl::check_parent_consistency(Entity_object_impl *parent,
                                                Object_id parent_id) const
{
  DBUG_ASSERT(parent);
  DBUG_ASSERT(parent->id() == parent_id);

  if (!parent)
  {
    my_error(ER_INVALID_DD_OBJECT, MYF(0), this->object_table().name().c_str(),
             "Invalid parent reference (NULL).");
    return true;
  }

  if (parent->id() != parent_id)
  {
    my_error(ER_INVALID_DD_OBJECT, MYF(0), this->object_table().name().c_str(),
             "Invalid parent ID");

    return true;
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////

}
