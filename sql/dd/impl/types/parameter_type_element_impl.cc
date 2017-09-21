/* Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

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

#include "sql/dd/impl/types/parameter_type_element_impl.h"

#include <stdio.h>
#include <string.h>

#include "m_ctype.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/dd/impl/raw/raw_record.h"            // Raw_record
#include "sql/dd/impl/tables/parameter_type_elements.h"// Parameter_type_elements
#include "sql/dd/impl/transaction_impl.h"          // Open_dictionary_tables_ctx
#include "sql/dd/impl/types/parameter_impl.h"      // Parameter_impl
#include "sql/dd/types/object_table.h"
#include "sql/dd/types/parameter_type_element.h"   // Parameter_type_element
#include "sql/dd/types/weak_object.h"
#include "sql/dd_table_share.h"                   // dd_get_mysql_charset
#include "sql/sql_const.h"                        // MAX_INTERVAL_VALUE_LENGTH

namespace dd {
class Object_key;
class Parameter;
}  // namespace dd

using dd::tables::Parameter_type_elements;

namespace dd {

///////////////////////////////////////////////////////////////////////////
// Parameter_type_element implementation.
///////////////////////////////////////////////////////////////////////////

const Object_table &Parameter_type_element::OBJECT_TABLE()
{
  return Parameter_type_elements::instance();
}

///////////////////////////////////////////////////////////////////////////

const Object_type &Parameter_type_element::TYPE()
{
  static Parameter_type_element_type s_instance;
  return s_instance;
}

///////////////////////////////////////////////////////////////////////////
// Parameter_type_element_impl implementation.
///////////////////////////////////////////////////////////////////////////

/* purecov: begin deadcode */
const Parameter &Parameter_type_element_impl::parameter() const
{
  return *m_parameter;
}
/* purecov: end */

///////////////////////////////////////////////////////////////////////////
bool Parameter_type_element_impl::validate() const
{
  if (!m_parameter)
  {
    my_error(ER_INVALID_DD_OBJECT,
             MYF(0),
             Parameter_type_element_impl::OBJECT_TABLE().name().c_str(),
             "No parameter associated with this object.");
    return true;
  }

  const CHARSET_INFO *cs= dd_get_mysql_charset(m_parameter->collation_id());
  DBUG_ASSERT(cs);
  const char* cstr= m_name.c_str();

  if (cs->cset->numchars(cs, cstr, cstr + strlen(cstr)) > MAX_INTERVAL_VALUE_LENGTH)
  {
    my_error(ER_TOO_LONG_SET_ENUM_VALUE,
             MYF(0),
             m_parameter->name().c_str());
    return true;
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////

bool Parameter_type_element_impl::restore_attributes(const Raw_record &r)
{
  if (check_parent_consistency(m_parameter,
        r.read_ref_id(Parameter_type_elements::FIELD_PARAMETER_ID)))
    return true;

  m_index= r.read_uint(Parameter_type_elements::FIELD_INDEX);
  m_name= r.read_str(Parameter_type_elements::FIELD_NAME);

  return false;
}

///////////////////////////////////////////////////////////////////////////

bool Parameter_type_element_impl::store_attributes(Raw_record *r)
{
  return r->store(Parameter_type_elements::FIELD_PARAMETER_ID,
                  m_parameter->id()) ||
         r->store(Parameter_type_elements::FIELD_INDEX, m_index) ||
         r->store(Parameter_type_elements::FIELD_NAME, m_name);
}

///////////////////////////////////////////////////////////////////////////

/* purecov: begin deadcode */
void Parameter_type_element_impl::debug_print(String_type &outb) const
{
  char outbuf[1024];
  sprintf(outbuf, "%s: "
    "name=%s, parameter_id={OID: %lld}, ordinal_position= %u",
    object_table().name().c_str(),
    m_name.c_str(), m_parameter->id(), m_index);
  outb= String_type(outbuf);
}
/* purecov: end */

///////////////////////////////////////////////////////////////////////////

/* purecov: begin deadcode */
Object_key *Parameter_type_element_impl::create_primary_key() const
{
  return Parameter_type_elements::create_primary_key(m_parameter->id(), m_index);
}
/* purecov: end */

bool Parameter_type_element_impl::has_new_primary_key() const
{
  return m_parameter->has_new_primary_key();
}

///////////////////////////////////////////////////////////////////////////

Parameter_type_element_impl::
Parameter_type_element_impl(const Parameter_type_element_impl &src,
                            Parameter_impl *parent)
  : Weak_object(src),
    m_name(src.m_name), m_index(src.m_index), m_parameter(parent)
{}

///////////////////////////////////////////////////////////////////////////
// Parameter_type_element_type implementation.
///////////////////////////////////////////////////////////////////////////

void Parameter_type_element_type::register_tables(Open_dictionary_tables_ctx *otx) const
{
  otx->add_table<Parameter_type_elements>();
}

///////////////////////////////////////////////////////////////////////////
}
