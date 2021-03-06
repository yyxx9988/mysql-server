/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

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

#include "sql/dd/upgrade/table.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <algorithm>
#include <string>

#include "lex_string.h"
#include "m_string.h"
#include "my_alloc.h"
#include "my_base.h"
#include "my_dbug.h"
#include "my_dir.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_loglevel.h"
#include "my_sys.h"
#include "my_user.h"                          // parse_user
#include "mysql/psi/psi_base.h"
#include "mysql/udf_registration_types.h"
#include "mysql_com.h"
#include "mysqld_error.h"                     // ER_*
#include "sql/dd/cache/dictionary_client.h"   // dd::cache::Dictionary_client
#include "sql/dd/dd_schema.h"                 // Schema_MDL_locker
#include "sql/dd/dd_table.h"                  // create_dd_user_table
#include "sql/dd/dd_trigger.h"                // dd::create_trigger
#include "sql/dd/dd_view.h"                   // create_view
#include "sql/dd/dictionary.h"
#include "sql/dd/impl/bootstrapper.h"         // execute_query
#include "sql/dd/properties.h"
#include "sql/dd/string_type.h"
#include "sql/dd/types/foreign_key.h"         // dd::Foreign_key
#include "sql/dd/types/table.h"               // dd::Table
#include "sql/dd/upgrade/global.h"
#include "sql/field.h"
#include "sql/handler.h"                      // legacy_db_type
#include "sql/key.h"
#include "sql/lock.h"                         // Tablespace_hash_set
#include "sql/log.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"                       // mysql_real_data_home
#include "sql/parse_file.h"                   // File_option
#include "sql/partition_element.h"
#include "sql/partition_info.h"               // partition_info
#include "sql/psi_memory_key.h"               // key_memory_TABLE
#include "sql/sp_head.h"                      // sp_head
#include "sql/sql_alter.h"
#include "sql/sql_base.h"                     // open_tables
#include "sql/sql_class.h"                    // THD
#include "sql/sql_const.h"
#include "sql/sql_lex.h"                      // new_empty_query_block
#include "sql/sql_list.h"
#include "sql/sql_parse.h"                    // check_string_char_length
#include "sql/sql_show.h"                     // view_store_options
#include "sql/sql_table.h"                    // build_tablename
#include "sql/sql_view.h"                     // mysql_create_view
#include "sql/system_variables.h"
#include "sql/table.h"                        // Table_check_intact
#include "sql/table_trigger_dispatcher.h"     // Table_trigger_dispatcher
#include "sql/thr_malloc.h"
#include "sql/transaction.h"                  // trans_commit
#include "sql/trigger.h"                      // Trigger
#include "sql/trigger_def.h"
#include "sql_string.h"
#include "thr_lock.h"

class Sroutine_hash_entry;
namespace dd {
class Schema;
class Table;
}  // namespace dd

namespace dd {
namespace upgrade {

/*
  Custom version of standard offsetof() macro which can be used to get
  offsets of members in class for non-POD types (according to the current
  version of C++ standard offsetof() macro can't be used in such cases and
  attempt to do so causes warnings to be emitted, OTOH in many cases it is
  still OK to assume that all instances of the class has the same offsets
  for the same members).

  This is temporary solution which should be removed once File_parser class
  and related routines are refactored.
*/

#define my_offsetof_upgrade(TYPE, MEMBER) \
        ((size_t)((char *)&(((TYPE *)0x10)->MEMBER) - (char*)0x10))



/**
  Class to handle loading and parsing of Triggers.
  This class is nececssary for loading triggers in
  case of upgrade from 5.7
*/

class Trigger_loader
{
public:
  static bool trg_file_exists(const char *db_name,
                              const char *table_name);

  static bool load_triggers(THD *thd,
                            MEM_ROOT *mem_root,
                            const char *db_name,
                            const char *table_name,
                            List<::Trigger> *triggers);
};


static const int TRG_NUM_REQUIRED_PARAMETERS= 8;

const LEX_STRING trg_file_type= { C_STRING_WITH_LEN("TRIGGERS") };

/**
  Structure representing contents of .TRG file.
*/

struct Trg_file_data
{
  // List of CREATE TRIGGER statements.
  List<LEX_STRING>  definitions;

  // List of 'sql mode' values.
  List<ulonglong> sql_modes;

  // List of 'definer' values.
  List<LEX_STRING>  definers_list;

  // List of client character set names.
  List<LEX_STRING> client_cs_names;

  // List of connection collation names.
  List<LEX_STRING> connection_cl_names;

  // List of database collation names.
  List<LEX_STRING> db_cl_names;

  // List of trigger creation time stamps
  List<longlong> created_timestamps;
};


/**
  Table of .TRG file field descriptors.
*/

static File_option trg_file_parameters[]=
{
  {
    { C_STRING_WITH_LEN("triggers") },
    my_offsetof_upgrade(struct Trg_file_data, definitions),
    FILE_OPTIONS_STRLIST
  },
  {
    { C_STRING_WITH_LEN("sql_modes") },
    my_offsetof_upgrade(struct Trg_file_data, sql_modes),
    FILE_OPTIONS_ULLLIST
  },
  {
    { C_STRING_WITH_LEN("definers") },
    my_offsetof_upgrade(struct Trg_file_data, definers_list),
    FILE_OPTIONS_STRLIST
  },
  {
    { C_STRING_WITH_LEN("client_cs_names") },
    my_offsetof_upgrade(struct Trg_file_data, client_cs_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { C_STRING_WITH_LEN("connection_cl_names") },
    my_offsetof_upgrade(struct Trg_file_data, connection_cl_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { C_STRING_WITH_LEN("db_cl_names") },
    my_offsetof_upgrade(struct Trg_file_data, db_cl_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { C_STRING_WITH_LEN("created") },
    my_offsetof_upgrade(struct Trg_file_data, created_timestamps),
    FILE_OPTIONS_ULLLIST
  },
  { { 0, 0 }, 0, FILE_OPTIONS_STRING }
};


static File_option sql_modes_parameters=
{
  { C_STRING_WITH_LEN("sql_modes") },
  my_offsetof_upgrade(struct Trg_file_data, sql_modes),
  FILE_OPTIONS_ULLLIST
};


/*
  Module private variables to be used in Trigger_loader::load_triggers().
*/

static LEX_STRING default_client_cs_name= NULL_STR;
static LEX_STRING default_connection_cl_name= NULL_STR;
static LEX_STRING default_db_cl_name= NULL_STR;


class Handle_old_incorrect_sql_modes_hook: public Unknown_key_hook
{
private:
  char *m_path;
public:
  Handle_old_incorrect_sql_modes_hook(char *file_path)
    : m_path(file_path)
  {};
  virtual bool process_unknown_string(const char *&unknown_key, uchar *base,
                                      MEM_ROOT *mem_root, const char *end);
};


/**
  Check if the TRG-file for the given table exists.

  @param db_name    name of schema
  @param table_name name of trigger

  @return true if TRG-file exists, false otherwise.
*/

bool Trigger_loader::trg_file_exists(const char *db_name,
                                     const char *table_name)
{
  char path[FN_REFLEN];
  build_table_filename(path, FN_REFLEN - 1, db_name, table_name, TRG_EXT, 0);

  if (access(path, F_OK))
  {
    if (errno == ENOENT)
      return false;
  }

  return true;
}


/**
  Load table triggers from .TRG file.

  @param [in]  thd                thread handle
  @param [in]  mem_root           MEM_ROOT pointer
  @param [in]  db_name            name of schema
  @param [in]  table_name         subject table name
  @param [out] triggers           pointer to the list where new Trigger
                                  objects will be inserted

  @return Operation status
    @retval true   Failure
    @retval false  Success
*/

bool Trigger_loader::load_triggers(THD *thd,
                                   MEM_ROOT *mem_root,
                                   const char *db_name,
                                   const char *table_name,
                                   List<::Trigger> *triggers)
{
  DBUG_ENTER("Trigger_loader::load_triggers");

  // Construct TRG-file name.

  char trg_file_path_buffer[FN_REFLEN];
  LEX_STRING trg_file_path;

  trg_file_path.length= build_table_filename(trg_file_path_buffer,
                                             FN_REFLEN - 1,
                                             db_name, table_name, TRG_EXT, 0);
  trg_file_path.str= trg_file_path_buffer;

  // The TRG-file exists so we got to load triggers.

  File_parser *parser=
    sql_parse_prepare(&trg_file_path, mem_root, true);

  if (!parser)
    DBUG_RETURN(true);

  if (!is_equal(&trg_file_type, parser->type()))
  {
    my_error(ER_WRONG_OBJECT, MYF(0), table_name, TRG_EXT + 1, "TRIGGER");
    DBUG_RETURN(true);
  }

  Handle_old_incorrect_sql_modes_hook sql_modes_hook(trg_file_path.str);

  Trg_file_data trg;

  if (parser->parse((uchar*) &trg,
                    mem_root,
                    trg_file_parameters,
                    TRG_NUM_REQUIRED_PARAMETERS,
                    &sql_modes_hook))
    DBUG_RETURN(true);

 if (trg.definitions.is_empty())
  {
    DBUG_ASSERT(trg.sql_modes.is_empty());
    DBUG_ASSERT(trg.definers_list.is_empty());
    DBUG_ASSERT(trg.client_cs_names.is_empty());
    DBUG_ASSERT(trg.connection_cl_names.is_empty());
    DBUG_ASSERT(trg.db_cl_names.is_empty());
    DBUG_RETURN(false);
  }

  // Make sure character set properties are filled.

  if (trg.client_cs_names.is_empty() ||
      trg.connection_cl_names.is_empty() ||
      trg.db_cl_names.is_empty())
  {
    if (!trg.client_cs_names.is_empty() ||
        !trg.connection_cl_names.is_empty() ||
        !trg.db_cl_names.is_empty())
    {
      my_error(ER_TRG_CORRUPTED_FILE, MYF(0),
               db_name,
               table_name);

      DBUG_RETURN(true);
    }

    LogErr(WARNING_LEVEL, ER_TRG_NO_CREATION_CTX, db_name, table_name);


    /*
      Backward compatibility: assume that the query is in the current
      character set.
    */

    lex_string_set(&default_client_cs_name,
                   thd->variables.character_set_client->csname);

    lex_string_set(&default_connection_cl_name,
                   thd->variables.collation_connection->name);

    lex_string_set(&default_db_cl_name,
                   thd->variables.collation_database->name);
  }

  LEX_CSTRING db_name_str= {db_name, strlen(db_name)};

  LEX_CSTRING table_name_str= {table_name, strlen(table_name)};

  List_iterator_fast<LEX_STRING> it_definition(trg.definitions);
  List_iterator_fast<sql_mode_t> it_sql_mode(trg.sql_modes);
  List_iterator_fast<LEX_STRING> it_definer(trg.definers_list);
  List_iterator_fast<LEX_STRING> it_client_cs_name(trg.client_cs_names);
  List_iterator_fast<LEX_STRING> it_connect_cl_name(trg.connection_cl_names);
  List_iterator_fast<LEX_STRING> it_db_cl_name(trg.db_cl_names);
  List_iterator_fast<longlong>  it_created_timestamps(trg.created_timestamps);

  while (true)
  {
    const LEX_STRING *definition= it_definition++;

    if (!definition)
      break;

    const sql_mode_t *sql_mode= it_sql_mode++;
    const LEX_STRING *definer= it_definer++;
    const LEX_STRING *client_cs_name= it_client_cs_name++;
    const LEX_STRING *connection_cl_name= it_connect_cl_name++;
    const LEX_STRING *db_cl_name= it_db_cl_name++;
    const longlong *created_timestamp= it_created_timestamps++;

    // Backward compatibility: use default settings if attributes are missing.

    if (!sql_mode)
      sql_mode= &global_system_variables.sql_mode;

    if (!definer)
    {
      // We dont know trigger name yet.
      LogErr(ERROR_LEVEL, ER_TRG_WITHOUT_DEFINER, table_name);
      return true;
    }

    if (!client_cs_name)
    {
      LogErr(WARNING_LEVEL, ER_TRG_NO_CLIENT_CHARSET, table_name);
      client_cs_name= &default_client_cs_name;
    }

    if (!connection_cl_name)
    {
      LogErr(WARNING_LEVEL, ER_DD_TRG_CONNECTION_COLLATION_MISSING,table_name);
      connection_cl_name= &default_connection_cl_name;
    }

    if (!db_cl_name)
    {
      LogErr(WARNING_LEVEL, ER_DD_TRG_DB_COLLATION_MISSING,table_name);
      db_cl_name= &default_db_cl_name;
    }

    char tmp_body_utf8[]= "temp_utf8_definition";
    LEX_CSTRING body_utf8= { tmp_body_utf8, strlen(tmp_body_utf8) };


    // Allocate space to hold username and hostname.
    char *pos= NULL;
    if (!(pos= static_cast<char*>(alloc_root(mem_root, USERNAME_LENGTH + 1))))
    {
      LogErr(ERROR_LEVEL, ER_DD_TRG_DEFINER_OOM, "User");
      return true;
    }

    LEX_STRING definer_user{pos, 0};

    if (!(pos= static_cast<char*>(alloc_root(mem_root, USERNAME_LENGTH + 1))))
    {
      LogErr(ERROR_LEVEL, ER_DD_TRG_DEFINER_OOM, "Host");
      return true;
    }

    LEX_STRING definer_host{pos, 0};


    // Parse user string to separate user name and host
    parse_user(definer->str, definer->length,
               definer_user.str,
               &definer_user.length,
               definer_host.str,
               &definer_host.length);

    LEX_CSTRING definer_user_name{ definer_user.str, definer_user.length };
    LEX_CSTRING definer_host_name{ definer_host.str, definer_host.length };

    // Set timeval to use for Created field.
    timeval timestamp_value;
    if (created_timestamp)
    {
      timestamp_value.tv_sec= static_cast<long>(*created_timestamp / 100);
      timestamp_value.tv_usec= (*created_timestamp % 100) * 10000;
    }
    else
    {
      // Trigger created before 5.7.2, set created value.
      timestamp_value= thd->query_start_timeval_trunc(2);
    }

    // Create temporary Trigger name to be fixed while parsing.
    // parse_triggers() will fix this.
    char temp_trigger_name[]= "temporary_trigger_name";
    LEX_CSTRING tmp_name= { temp_trigger_name, strlen(temp_trigger_name) };

    // Create definition as LEX_CSTRING
    LEX_CSTRING orig_definition= { definition->str, definition->length };

    // Create client_character_set as LEX_CSTRING
    LEX_CSTRING client_cs= { client_cs_name->str, client_cs_name->length };

    // Create connection_collation as LEX_CSTRING
    LEX_CSTRING cn_cl= { connection_cl_name->str, connection_cl_name->length };

    // Create database_collation as LEX_CSTRING
    LEX_CSTRING db_cl= { db_cl_name->str, db_cl_name->length };

    // Create a new trigger instance.
    ::Trigger *t= ::Trigger::create_from_dd(
            mem_root,
            tmp_name,
            db_name_str,
            table_name_str,
            orig_definition,
            body_utf8,
            *sql_mode,
            definer_user_name,
            definer_host_name,
            client_cs,
            cn_cl,
            db_cl,
            enum_trigger_event_type::TRG_EVENT_MAX,
            enum_trigger_action_time_type::TRG_ACTION_MAX,
            0,
            timestamp_value);

    /*
      NOTE: new trigger object is not fully initialized here.
      Initialization of definer, trigger name, action time, action event
      will be done in parse_triggers().
    */
    if (triggers->push_back(t, mem_root))
    {
      destroy(t);
      DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}


/**
  Trigger BUG#14090 compatibility hook.

  @param[in,out] unknown_key       reference on the line with unknown
    parameter and the parsing point
  @param[in]     base              base address for parameter writing
    (structure like TABLE)
  @param[in]     mem_root          MEM_ROOT for parameters allocation
  @param[in]     end               the end of the configuration

  @note
    NOTE: this hook process back compatibility for incorrectly written
    sql_modes parameter (see BUG#14090).

  @retval
    false OK
  @retval
    true  Error
*/

#define INVALID_SQL_MODES_LENGTH 13

bool Handle_old_incorrect_sql_modes_hook::process_unknown_string(
  const char *&unknown_key,
  uchar *base,
  MEM_ROOT *mem_root,
  const char *end)
{
  DBUG_ENTER("Handle_old_incorrect_sql_modes_hook::process_unknown_string");
  DBUG_PRINT("info", ("unknown key: %60s", unknown_key));

  if (unknown_key + INVALID_SQL_MODES_LENGTH + 1 < end &&
      unknown_key[INVALID_SQL_MODES_LENGTH] == '=' &&
      !memcmp(unknown_key, STRING_WITH_LEN("sql_modes")))
  {
    const char *ptr= unknown_key + INVALID_SQL_MODES_LENGTH + 1;

    DBUG_PRINT("info", ("sql_modes affected by BUG#14090 detected"));
    LogErr(WARNING_LEVEL, ER_OLD_FILE_FORMAT, m_path, "TRIGGER");
    if (get_file_options_ulllist(ptr, end, unknown_key, base,
                                 &sql_modes_parameters, mem_root))
    {
      DBUG_RETURN(true);
    }
    /*
      Set parsing pointer to the last symbol of string (\n)
      1) to avoid problem with \0 in the junk after sql_modes
      2) to speed up skipping this line by parser.
    */
    unknown_key= ptr - 1;
  }
  DBUG_RETURN(false);
}


/**
  RAII to handle MDL locks while upgrading.
*/

class Upgrade_MDL_guard
{
  MDL_ticket *m_mdl_ticket_schema;
  MDL_ticket *m_mdl_ticket_table;
  bool m_tablespace_lock;

  THD *m_thd;
public:
  bool acquire_lock(const String_type &db_name, const String_type &table_name)
  {
    return dd::acquire_exclusive_schema_mdl(m_thd, db_name.c_str(),
                                            false, &m_mdl_ticket_schema) ||
           dd::acquire_exclusive_table_mdl(m_thd, db_name.c_str(),
                                           table_name.c_str(),
                                           false, &m_mdl_ticket_table);
  }
  bool acquire_lock_tablespace(Tablespace_hash_set *tablespace_names)
  {
    m_tablespace_lock= true;
    return lock_tablespace_names(m_thd, tablespace_names,
                                 m_thd->variables.lock_wait_timeout);
  }

  Upgrade_MDL_guard(THD *thd)
    : m_mdl_ticket_schema(nullptr), m_mdl_ticket_table(nullptr),
      m_tablespace_lock(false), m_thd(thd)
  {}
  ~Upgrade_MDL_guard()
  {
    if (m_mdl_ticket_schema != nullptr)
      dd::release_mdl(m_thd, m_mdl_ticket_schema);
    if ((m_mdl_ticket_table != nullptr) || m_tablespace_lock)
      m_thd->mdl_context.release_transactional_locks();
  }
};


/**
  RAII to handle cleanup after table upgrading.
*/

class Table_upgrade_guard
{
  THD *m_thd;
  TABLE *m_table;
  MEM_ROOT *m_mem_root;
  sql_mode_t m_sql_mode;
  handler *m_handler;
  bool m_is_table_open;
  LEX *m_lex_saved;
public:

  void update_mem_root(MEM_ROOT *mem_root)
  {
    m_mem_root= mem_root;
  }

  void update_handler(handler *handler)
  {
    m_handler= handler;
  }

  void update_lex(LEX *lex)
  {
    m_lex_saved= lex;
  }

  void set_is_table_open(bool param)
  {
    m_is_table_open= param;
  }

  Table_upgrade_guard(THD *thd, TABLE *table, MEM_ROOT *mem_root)
    :  m_thd(thd), m_table(table), m_mem_root(mem_root), m_handler(nullptr),
       m_is_table_open(false), m_lex_saved(nullptr)
  {
    m_sql_mode= m_thd->variables.sql_mode;
    m_thd->variables.sql_mode= m_sql_mode;
  }

  ~Table_upgrade_guard()
  {
    m_thd->variables.sql_mode= m_sql_mode;
    m_thd->work_part_info= 0;

    // Free item list for partitions
    if (m_table->s->m_part_info)
      free_items(m_table->s->m_part_info->item_free_list);

    // Restore thread lex
    if (m_lex_saved != nullptr)
    {
      lex_end(m_thd->lex);
      m_thd->lex= m_lex_saved;
    }

    /*
      Free item list for generated columns
      Items being freed were allocated by fix_generated_columns_for_upgrade(),
      and TABLE instance might have its own items allocated which will be freed
      by closefrm() call.
    */
    if (m_table->s->field)
    {
      for (Field **ptr=m_table->s->field ; *ptr ; ptr++)
      {
        if ((*ptr)->gcol_info)
          free_items((*ptr)->gcol_info->item_free_list);
      }
    }

    // Close the table. It was opened using ha_open for FK information.
    if (m_is_table_open)
    {
      (void) closefrm(m_table, false);
    }

    free_table_share(m_table->s);

    destroy(m_handler);
    /*
      Make a copy of mem_root as TABLE object is allocated within its
      own mem_root and free_root() updates its argument.
    */
    MEM_ROOT m_root= std::move(*m_mem_root);
    free_root(&m_root, MYF(0));
  }
};


/**
  Fill HA_CREATE_INFO from TABLE_SHARE.
*/

static void  fill_create_info_for_upgrade(HA_CREATE_INFO *create_info,
                                          const TABLE *table)
{
  /*
    Storage Engine names will be resolved when reading .frm file.
    We can assume here that SE is present and initialized.
  */
  create_info->db_type= table->s->db_type();

  create_info->init_create_options_from_share(table->s, 0);

  create_info->row_type= table->s->row_type;

  // DD framework handles only these options
  uint db_create_options= table->s->db_create_options;
  db_create_options &= (HA_OPTION_PACK_RECORD|
                        HA_OPTION_PACK_KEYS|HA_OPTION_NO_PACK_KEYS|
                        HA_OPTION_CHECKSUM|HA_OPTION_NO_CHECKSUM|
                        HA_OPTION_DELAY_KEY_WRITE|HA_OPTION_NO_DELAY_KEY_WRITE|
                        HA_OPTION_STATS_PERSISTENT|HA_OPTION_NO_STATS_PERSISTENT);
  create_info->table_options= db_create_options;
}



static const int REQUIRED_VIEW_PARAMETERS= 12;

/*
  Table of VIEW .frm field descriptors

  Note that one should NOT change the order for this,
  as it's used by parse().
*/
static File_option view_parameters[]=
{{{ C_STRING_WITH_LEN("query")},
  my_offsetof_upgrade(TABLE_LIST, select_stmt),
  FILE_OPTIONS_ESTRING},
 {{ C_STRING_WITH_LEN("updatable")},
  my_offsetof_upgrade(TABLE_LIST, updatable_view),
  FILE_OPTIONS_ULONGLONG},
 {{ C_STRING_WITH_LEN("algorithm")},
  my_offsetof_upgrade(TABLE_LIST, algorithm),
  FILE_OPTIONS_ULONGLONG},
 {{ C_STRING_WITH_LEN("definer_user")},
  my_offsetof_upgrade(TABLE_LIST, definer.user),
  FILE_OPTIONS_STRING},
 {{ C_STRING_WITH_LEN("definer_host")},
  my_offsetof_upgrade(TABLE_LIST, definer.host),
  FILE_OPTIONS_STRING},
 {{ C_STRING_WITH_LEN("suid")},
  my_offsetof_upgrade(TABLE_LIST, view_suid),
  FILE_OPTIONS_ULONGLONG},
 {{ C_STRING_WITH_LEN("with_check_option")},
  my_offsetof_upgrade(TABLE_LIST, with_check),
  FILE_OPTIONS_ULONGLONG},
 {{ C_STRING_WITH_LEN("timestamp")},
  my_offsetof_upgrade(TABLE_LIST, timestamp),
  FILE_OPTIONS_TIMESTAMP},
 {{ C_STRING_WITH_LEN("source")},
  my_offsetof_upgrade(TABLE_LIST, source),
  FILE_OPTIONS_ESTRING},
 {{(char*) STRING_WITH_LEN("client_cs_name")},
  my_offsetof_upgrade(TABLE_LIST, view_client_cs_name),
  FILE_OPTIONS_STRING},
 {{(char*) STRING_WITH_LEN("connection_cl_name")},
  my_offsetof_upgrade(TABLE_LIST, view_connection_cl_name),
  FILE_OPTIONS_STRING},
 {{(char*) STRING_WITH_LEN("view_body_utf8")},
  my_offsetof_upgrade(TABLE_LIST, view_body_utf8),
  FILE_OPTIONS_ESTRING},
 {{NullS, 0},                   0,
  FILE_OPTIONS_STRING}
};


/**
  Create the view in DD without its column and dependency
  information.

  @param[in] thd       Thread handle.
  @param[in] view_ref  TABLE_LIST to store view data.

  @retval false  ON SUCCESS
  @retval true   ON FAILURE
*/
static bool create_unlinked_view(THD *thd,
                                 TABLE_LIST *view_ref)
{
  SELECT_LEX *backup_select= thd->lex->select_lex;
  TABLE_LIST *saved_query_tables= thd->lex->query_tables;
  SQL_I_List<Sroutine_hash_entry> saved_sroutines_list;
  // For creation of view without column information.
  SELECT_LEX select(nullptr, nullptr);

  // Backup
  thd->lex->select_lex= &select;
  thd->lex->query_tables= NULL;
  thd->lex->sroutines_list.save_and_clear(&saved_sroutines_list);

  const dd::Schema *schema= nullptr;
  if (thd->dd_client()->acquire(view_ref->db, &schema))
    return true;
  DBUG_ASSERT(schema != nullptr); // Should be impossible during upgrade.

  // Disable autocommit option in thd variable
  Disable_autocommit_guard autocommit_guard(thd);

  bool result= dd::create_view(thd, *schema, view_ref);

  Disable_gtid_state_update_guard disabler(thd);
  if (result)
  {
    trans_rollback_stmt(thd);
    // Full rollback in case we have THD::transaction_rollback_request.
    trans_rollback(thd);
  }
  else
    result= trans_commit_stmt(thd) || trans_commit(thd);

  // Restore
  thd->lex->select_lex= backup_select;
  thd->lex->sroutines_list.push_front(&saved_sroutines_list);
  thd->lex->query_tables= saved_query_tables;

  return result;
}


/**
  Construct ALTER VIEW statement to fix the column list
  and dependency information but retains the previous
  view defintion entry in DD.

  @param[in]  thd       Thread handle.
  @param[in]  view_ref  TABLE_LIST to store view data.
  @param[out] str       String object to store view definition.
  @param[in]  db_name   database name.
  @param[in]  view_name view name.
  @param[in]  cs        Charset Information.
*/
static void create_alter_view_stmt(THD *thd,
                                   TABLE_LIST *view_ref,
                                   String *str,
                                   const String_type &db_name,
                                   const String_type &view_name,
                                   const CHARSET_INFO *cs)
{
  str->append(STRING_WITH_LEN("ALTER "));
  view_store_options(thd, view_ref, str);
  str->append(STRING_WITH_LEN("VIEW "));
  append_identifier(thd, str, db_name.c_str(), db_name.length());
  str->append('.');
  append_identifier(thd, str, view_name.c_str(), view_name.length());
  str->append(STRING_WITH_LEN(" AS "));
  str->append(view_ref->select_stmt.str, view_ref->select_stmt.length, cs);
  if (view_ref->with_check != VIEW_CHECK_NONE)
  {
    if (view_ref->with_check == VIEW_CHECK_LOCAL)
      str->append(STRING_WITH_LEN(" WITH LOCAL CHECK OPTION"));
    else
      str->append(STRING_WITH_LEN(" WITH CASCADED CHECK OPTION"));
  }
}


/**
  Finalize upgrading view by fixing column data, table and routines
  dependency.
  View will be marked invalid if ALTER VIEW statement fails on the view.

  @param[in] thd                     Thread handle.
  @param[in] view_ref                TABLE_LIST with view data.
  @param[in] db_name                 database name.
  @param[in] view_name               view name.
  @param[in] mem_root                MEM_ROOT to handle memory allocations.

  @retval false  ON SUCCESS
  @retval true   ON FAILURE

*/
static bool fix_view_cols_and_deps(THD *thd, TABLE_LIST *view_ref,
                                   const String_type &db_name,
                                   const String_type &view_name,
                                   MEM_ROOT *mem_root)
{
  bool error= false;

  const CHARSET_INFO *client_cs= thd->variables.character_set_client;
  const CHARSET_INFO *cs= thd->variables.collation_connection;
  const CHARSET_INFO *m_client_cs, *m_connection_cl;

  /*
    Charset has beed fixed in migrate_view_to_dd().
    resolve functions should never fail here.
  */
  resolve_charset(view_ref->view_client_cs_name.str,
                  system_charset_info,
                  &m_client_cs);

  resolve_collation(view_ref->view_connection_cl_name.str,
                    system_charset_info,
                    &m_connection_cl);

  thd->variables.character_set_client= m_client_cs;
  thd->variables.collation_connection= m_connection_cl;
  thd->update_charset();

  MEM_ROOT *m_mem_root= thd->mem_root;
  thd->mem_root= mem_root;

  // Switch off modes which can prevent normal parsing of VIEW.
  Sql_mode_parse_guard parse_guard(thd);

  String full_view_definition((char *)0, 0, m_connection_cl);
  create_alter_view_stmt(thd, view_ref, &full_view_definition,
                         db_name, view_name, m_connection_cl);

  String db_query;
  db_query.append(STRING_WITH_LEN("USE "));
  append_identifier(thd, &db_query, db_name.c_str(), db_name.length());

  String_type change_db_query(db_query.ptr(), db_query.length());
  error= execute_query(thd, change_db_query);

  // Execute ALTER view statement to create the view dependency entry in DD.
  String_type query(full_view_definition.ptr(), full_view_definition.length());
  if (!error)
    error= execute_query(thd, query);

  // Disable autocommit option in thd variable
  Disable_autocommit_guard autocommit_guard(thd);

  /*
    If there is an error in ALTERing view, mark it as invalid and proceed
    with upgrade.
  */
  if (error)
  {
    /*
      Do not print warning if view belongs to sys schema. Sys schema views will
      get fixed when mysql_upgrade is executed.
    */
    if (db_name != "sys")
      LogErr(WARNING_LEVEL, ER_DD_CANT_RESOLVE_VIEW,
             db_name.c_str(), view_name.c_str());
    update_view_status(thd, db_name.c_str(), view_name.c_str(), false, true);
    error= false;
  }

  // Restore variables
  thd->variables.character_set_client= client_cs;
  thd->variables.collation_connection= cs;
  thd->update_charset();
  thd->mem_root= m_mem_root;

  return error;
}


/**
  Create an entry in the DD for the view.

  @param[in]  thd                       Thread handle.
  @param[in]  frm_context               Structure to hold view definition
                                        read from frm file.
  @param[in]  db_name                   database name.
  @param[in]  view_name                 view name.
  @param[in]  mem_root                  MEM_ROOT to handle memory allocations.
  @param [in] is_fix_view_cols_and_deps Flag to create view
                                        with dependency information.
  @retval false  ON SUCCESS
  @retval true   ON FAILURE
*/

static bool migrate_view_to_dd(THD *thd,
                               const FRM_context &frm_context,
                               const String_type &db_name,
                               const String_type &view_name,
                               MEM_ROOT *mem_root,
                               bool is_fix_view_cols_and_deps)
{
  TABLE_LIST table_list;

  table_list.init_one_table(db_name.c_str(), db_name.length(),
                            view_name.c_str(), view_name.length(),
                            view_name.c_str(), TL_READ);

  // Initialize timestamp
  table_list.timestamp.str= table_list.timestamp_buffer;

  // Prepare default values for old format
  table_list.view_suid= TRUE;
  table_list.definer.user.str= table_list.definer.host.str= 0;
  table_list.definer.user.length= table_list.definer.host.length= 0;

  if (frm_context.view_def->parse(reinterpret_cast<uchar*>(&table_list),
                               mem_root,
                               view_parameters, REQUIRED_VIEW_PARAMETERS,
                               &file_parser_dummy_hook))
  {
    LogErr(ERROR_LEVEL, ER_PARSING_VIEW, db_name.c_str(), view_name.c_str());
    return true;
  }

  // Check old format view .frm file
  if (!table_list.definer.user.str)
  {
    LogErr(WARNING_LEVEL, ER_DD_VIEW_WITHOUT_DEFINER,
           db_name.c_str(),  view_name.c_str());
    get_default_definer(thd, &table_list.definer);
  }


  /*
    Check client character_set and connection collation.
    Throw a warning if there is no or unknown cs name.
    Print warning in error log only once.
  */
  bool invalid_ctx= false;

  // Check for blank creation context
  if (table_list.view_client_cs_name.str == nullptr ||
      table_list.view_connection_cl_name.str == nullptr)
  {
    // Print warning only once in the error log.
    if (!is_fix_view_cols_and_deps)
      LogErr(WARNING_LEVEL, ER_VIEW_NO_CREATION_CTX,
             db_name.c_str(), view_name.c_str());
    invalid_ctx= true;
  }

  // Check for valid character set.
  const CHARSET_INFO *cs= nullptr;
  if (!invalid_ctx)
  {
    invalid_ctx = resolve_charset(table_list.view_client_cs_name.str,
                                  system_charset_info,
                                  &cs);

    invalid_ctx |= resolve_collation(table_list.view_connection_cl_name.str,
                                     system_charset_info,
                                     &cs);

    // Print warning only once in the error log.
    if (!is_fix_view_cols_and_deps && invalid_ctx)
      LogErr(WARNING_LEVEL, ER_VIEW_UNKNOWN_CHARSET_OR_COLLATION,
             db_name.c_str(), view_name.c_str(),
             table_list.view_client_cs_name.str,
             table_list.view_connection_cl_name.str);
  }

  // Set system_charset_info for view.
  if (invalid_ctx)
  {
    cs= system_charset_info;
    size_t cs_length= strlen(cs->csname);
    size_t length= strlen(cs->name);

    table_list.view_client_cs_name.str= strmake_root(mem_root,
                                                      cs->csname,
                                                      cs_length);
    table_list.view_client_cs_name.length= cs_length;

    table_list.view_connection_cl_name.str= strmake_root(mem_root,
                                                          cs->name,
                                                          length);
    table_list.view_connection_cl_name.length= length;

    if (table_list.view_client_cs_name.str == nullptr ||
        table_list.view_connection_cl_name.str == nullptr)
    {
      LogErr(ERROR_LEVEL, ER_DD_VIEW_CANT_ALLOC_CHARSET,
             db_name.c_str(), view_name.c_str());
      return true;
    }
  }

  // View is already created, we are recreating it now.
  if (is_fix_view_cols_and_deps)
  {
    if (fix_view_cols_and_deps(thd, &table_list, db_name, view_name, mem_root))
    {
      LogErr(ERROR_LEVEL, ER_DD_VIEW_CANT_CREATE,
             db_name.c_str(), view_name.c_str());
      return true;
    }
  }
  else
  {
    /*
      Create view without making entry in mysql.columns,
      mysql.view_table_usage and mysql.view_routine_usage.
    */
    if (create_unlinked_view(thd, &table_list))
    {
      LogErr(ERROR_LEVEL, ER_PARSING_VIEW, db_name.c_str(), view_name.c_str());
      return true;
    }
  }
  return false;
}


/**
   Create partition information for upgrade.
   This function uses the same method to create partition information
   as done by open_table_from_share().
*/

static bool fill_partition_info_for_upgrade(THD *thd,
                                            TABLE_SHARE *share,
                                            const FRM_context *frm_context,
                                            TABLE *table)
{
  thd->work_part_info= nullptr;

  // If partition information is present in TABLE_SHARE
  if (share->partition_info_str_len && table->file)
  {
    // Parse partition expression and create Items.
    if (unpack_partition_info(thd, table, share,
        frm_context->default_part_db_type, false))
      return true;

    // dd::create_dd_user_table() uses thd->part_info to get partition values.
    thd->work_part_info= table->part_info;
    // This assignment is necessary to free the partition_info
    share->m_part_info= table->part_info;
    /*
      For normal TABLE instances, free_items() is called by closefrm().
      For this scenario, free_items() will be called by destructor of
      Table_upgrade_guard.
    */
    share->m_part_info->item_free_list= table->part_info->item_free_list;
  }
  return false;
}


/**
  Add triggers to table
*/

static bool add_triggers_to_table(THD *thd,
                                  TABLE *table,
                                  const String_type &schema_name,
                                  const String_type &table_name)
{
  List<::Trigger> m_triggers;
  if (Trigger_loader::trg_file_exists(schema_name.c_str(),
                                      table_name.c_str()))
  {
    if (Trigger_loader::load_triggers(thd,
                                      &table->mem_root,
                                      schema_name.c_str(),
                                      table_name.c_str(),
                                      &m_triggers))
    {
      LogErr(WARNING_LEVEL, ER_DD_TRG_FILE_UNREADABLE, table_name.c_str());
      return true;
    }
    Table_trigger_dispatcher *d= Table_trigger_dispatcher::create(table);

    d->parse_triggers(thd, &m_triggers, true);
    if (d->check_for_broken_triggers())
    {
      LogErr(WARNING_LEVEL, ER_TRG_CANT_PARSE, table_name.c_str());
      return true;
    }


    List_iterator<::Trigger> it(m_triggers);
    /*
      Fix the order column for the execution of Triggers with
      same action event and same action timing. .TRG filed used to handle
      this by storing the triggers in the order of their execution.
    */

    // Get 1st Trigger
    ::Trigger *t= it++;

    // If no Trigger found, return
    if (!t)
      return false;

    ulonglong order= 1;
    enum_trigger_event_type t_type= t->get_event();
    enum_trigger_action_time_type t_time= t->get_action_time();

    // Set order for 1st Trigger as 1.
    t->set_action_order(order);
    order= order + 1;

    // Set action order for rest of the Triggers.
    while (true)
    {
      ::Trigger *t= it++;

      if (!t)
        break;

      /*
        events of the same type and timing always go in one group according
        to their action order.
      */
      assert(t->get_event() >= t_type && (t->get_event() > t_type ||
                                          t->get_action_time() >= t_time));

      // We found next trigger with same action event and same action time.
      if (t->get_event() == t_type &&
          t->get_action_time() == t_time)
      {
        // Set action order for Trigger
        t->set_action_order(order);
        // Increment the value of action order
        order= order + 1;
        continue;
      }
      // If action event OR action time OR both changes for the next trigger.
      else
      {
        // Reset action order value to 1.
        order= 1;
        // Set "1" as the action order.
        t->set_action_order(order);
        // Increment the value of action order
        order= order + 1;
        // Reset values of t_type and t_time
        t_type= t->get_event();
        t_time= t->get_action_time();
        continue;
      }
    } // End of while loop

    // Set Iterator to the beginning
    it.rewind();

    // Create entry in DD table for each trigger.
    while (true)
    {
      ::Trigger *t= it++;

      if (!t)
        break;

      Disable_gtid_state_update_guard disabler(thd);

      // Ordering of Triggers is taken care above, pass dummy arguments here.
      LEX_CSTRING anchor_trigger_name{0, 0};
      if (dd::create_trigger(thd, t,
                             enum_trigger_order_type::TRG_ORDER_NONE,
                             anchor_trigger_name))
      {
        trans_rollback_stmt(thd);
        // Full rollback in case we have THD::transaction_rollback_request.
        trans_rollback(thd);
        return true;
      }
      // dd::create_trigger() does not commit transaction
      if (trans_commit_stmt(thd) || trans_commit(thd))
      {
        LogErr(ERROR_LEVEL, ER_DD_TRG_CANT_ADD,
               t->get_db_name().str, t->get_trigger_name().str);
        return true;
      }

      //Cleanup for Trigger
      sp_head *sp= t->get_sp();
      sp_head *saved_sphead= thd->lex->sphead;
      thd->lex->sphead= sp;
      sp->m_parser_data.finish_parsing_sp_body(thd);
      thd->lex->sphead= saved_sphead;
      sp_head::destroy(sp);

    } //End of while loop
  } //End of If condition to check Trigger existance
  return false;
}


/**
  Fix generated columns.

  @param[in]  thd            Thread handle.
  @param[in]  table          TABLE object.
  @param[in]  create_fields  List of Create_fields

  @retval false  ON SUCCESS
  @retval true   ON FAILURE

*/

static bool fix_generated_columns_for_upgrade(THD *thd,
                                              TABLE *table,
                                              List<Create_field> &create_fields)
{
  Create_field *sql_field;
  bool error_reported= FALSE;
  bool error= false;

  if (table->s->vfields)
  {
    List_iterator<Create_field> itc(create_fields);
    Field **field_ptr;

    for (field_ptr= table->s->field;
         (sql_field=itc++);
         field_ptr++)
    {
      // Field has generated col information.
      if (sql_field->gcol_info && (*field_ptr)->gcol_info)
      {
        if (unpack_gcol_info(thd, table, *field_ptr,
                             FALSE, &error_reported))
        {
          error= true;
          break;
        }
        sql_field->gcol_info->expr_item= (*field_ptr)->gcol_info->expr_item;
      }
    }
  }

  return error;
}


/**
  Call handler API to get storate engine specific metadata. Storage Engine
  should fill tablespace information, Foreign key information, partition
  information and correct row type.

  @param[in]    thd             Thread Handle
  @param[in]    schema_name     Name of schema
  @param[in]    table_name      Name of table
  @param[in]    table           TABLE object
  @param[in]    skip_error      Skip error in case of innodb stats table.

  @retval false  ON SUCCESS
  @retval true   ON FAILURE
*/

static bool
set_se_data_for_user_tables(THD *thd,
                            const String_type &schema_name,
                            const String_type &table_name,
		            TABLE *table,
                            bool skip_error)
{
  Disable_autocommit_guard autocommit_guard(thd);
  dd::Schema_MDL_locker mdl_locker(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  const dd::Schema *sch= nullptr;
  if (thd->dd_client()->acquire<dd::Schema>(schema_name.c_str(), &sch))
    return true;

  dd::Table *table_def= nullptr;
  if (thd->dd_client()->acquire_for_modification(schema_name.c_str(),
                                                 table_name.c_str(),
                                                 &table_def))
  {
    // Error is reported by the dictionary subsystem.
    return true;
  }

  if (!table_def)
  {
    /*
       Should never hit this case as the caller of this function stores
       the information in dictionary.
    */
    LogErr(ERROR_LEVEL, ER_DD_CANT_FETCH_TABLE_DATA,
           table_name.c_str(), schema_name.c_str());
    return true;
  }

  if (table->file->ha_upgrade_table(thd,
                                    schema_name.c_str(),
                                    table_name.c_str(),
                                    table_def,
                                    table))
  {
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    // Ignore error in upgrading stats tables.
    if (skip_error)
      return false;
    else
      return true;
  }

  if(thd->dd_client()->update<dd::Table>(table_def))
  {
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return true;
  }

  return trans_commit_stmt(thd) || trans_commit(thd);
}


/**
  Set names of parent keys (unique constraint names matching FK
  in parent tables) for the FKs in which table participates.

  @param  thd         Thread context.
  @param  schema_name Name of schema.
  @param  table_name  Name of table.
  @param  hton        Table's handlerton.

  @retval false - Success.
  @retval true  - Failure.
*/

static bool fix_fk_parent_key_names(THD *thd,
                                    const String_type &schema_name,
                                    const String_type &table_name,
                                    handlerton *hton)
{
  if (!(hton->flags & HTON_SUPPORTS_FOREIGN_KEYS))
  {
    // Shortcut. No need to process FKs for engines which don't support them.
    return false;
  }

  Disable_autocommit_guard autocommit_guard(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  dd::Table *table_def= nullptr;

  if (thd->dd_client()->acquire_for_modification(schema_name.c_str(),
                                                 table_name.c_str(),
                                                 &table_def))
  {
    // Error is reported by the dictionary subsystem.
    return true;
  }

  if (!table_def)
  {
    /*
      Should never hit this case as the caller of this function stores
      the information in dictionary.
    */
    LogErr(ERROR_LEVEL, ER_DD_CANT_FETCH_TABLE_DATA,
           table_name.c_str(), schema_name.c_str());
    return true;
  }

  for (dd::Foreign_key *fk : *(table_def->foreign_keys()))
  {
    const dd::Table *parent_table_def= nullptr;

    if (my_strcasecmp(table_alias_charset,
                      fk->referenced_table_schema_name().c_str(),
                      schema_name.c_str()) == 0 &&
        my_strcasecmp(table_alias_charset,
                      fk->referenced_table_name().c_str(),
                      table_name.c_str()) == 0)
    {
      // This FK references the same table as on which it is defined.
      parent_table_def= table_def;
    }
    else
    {
      if (thd->dd_client()->acquire(fk->referenced_table_schema_name().c_str(),
                                    fk->referenced_table_name().c_str(),
                                    &parent_table_def))
        return true;
    }

    if (parent_table_def == nullptr)
    {
      /*
        This is legal situaton. Parent table was not upgraded yet or
        simply doesn't exist. In the former case our FKs will be
        updated with the correct parent key names once parent table
        is upgraded.
      */
    }
    else
    {
      const char *parent_key_name= find_fk_parent_key(parent_table_def, fk);
      // Note: If the key returned above is "", this is interpreted as NULL
      // when storing the value to the DD tables.
      fk->set_unique_constraint_name(parent_key_name);
    }
  }

  /*
    Adjust parent key names for FKs belonging to already upgraded tables,
    which reference the table being upgraded here. Also adjust the
    foreign key parent collection, both for this table and for other
    tables being referenced by this one.
  */
  if (adjust_fk_children_after_parent_def_change(thd,
                                                 schema_name.c_str(),
                                                 table_name.c_str(),
                                                 hton,
                                                 table_def,
                                                 false) || // Don't invalidate
                                                           // TDC we don't have
                                                           // proper MDL.
      adjust_fk_parents(thd, schema_name.c_str(), table_name.c_str(),
                        true, nullptr))
  {
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return true;
  }

  if (thd->dd_client()->update(table_def))
  {
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return true;
  }

  return trans_commit_stmt(thd) || trans_commit(thd);
}


/**
  Read .frm files and enter metadata for tables/views.
*/

static bool migrate_table_to_dd(THD *thd,
                                const String_type &schema_name,
                                const String_type &table_name,
                                bool is_fix_view_cols_and_deps)
{
  int error= 0;
  FRM_context frm_context;
  TABLE_SHARE share;
  TABLE *table= nullptr;
  Field **ptr,*field;
  handler *file= nullptr;

  char path[FN_REFLEN + 1];
  bool was_truncated= false;
  build_table_filename(path, sizeof(path) - 1 - reg_ext_length,
                       schema_name.c_str(),
                       table_name.c_str(), "", 0,
                       &was_truncated);

  if (was_truncated)
  {
    LogErr(ERROR_LEVEL, ER_IDENT_CAUSES_TOO_LONG_PATH, sizeof(path) - 1, path);
    return true;
  }

  // Create table share for tables and views.
  if (create_table_share_for_upgrade(thd,
                                     path,
                                     &share,
                                     &frm_context,
                                     schema_name.c_str(),
                                     table_name.c_str(),
                                     is_fix_view_cols_and_deps))
  {
    LogErr(ERROR_LEVEL, ER_CANT_CREATE_TABLE_SHARE_FROM_FRM,
           table_name.c_str());
    return true;
  }

  /*
     Acquire mdl lock before upgrading.
     Don't acquire mdl lock if fixing dummy views.
  */
  Upgrade_MDL_guard mdl_guard(thd);
  if (mdl_guard.acquire_lock(schema_name, table_name))
  {
    free_table_share(&share);
    LogErr(ERROR_LEVEL, ER_CANT_LOCK_TABLE,
           schema_name.c_str(), table_name.c_str());
    return true;
  }

  {
    // Initialize TABLE mem_root
    MEM_ROOT mem_root;
    init_sql_alloc(key_memory_TABLE,
                   &mem_root, TABLE_ALLOC_BLOCK_SIZE, 0);

    // Make a new TABLE object
    if (!(table= new (&mem_root) TABLE()))
    {
      free_table_share(&share);
      LogErr(ERROR_LEVEL, ER_CANT_ALLOC_TABLE_OBJECT);
      return true;
    }

    // Fix pointers in TABLE, TABLE_SHARE
    memset(table, 0, sizeof(*table));
    table->s= &share;
    table->in_use= thd;
    table->mem_root= std::move(mem_root);
  }

  // Object to handle cleanup.
  Table_upgrade_guard table_guard(thd, table, &table->mem_root);

  // Dont upgrade tables, we are fixing dependency for views.
  if (!share.is_view && is_fix_view_cols_and_deps)
    return false;

  if (share.is_view)
    return (migrate_view_to_dd(thd, frm_context, schema_name,
                               table_name, &table->mem_root,
                               is_fix_view_cols_and_deps));

  // Get the handler
  if (!(file= get_new_handler(&share,
                              share.partition_info_str_len != 0,
                              &table->mem_root,
                              share.db_type())))
  {
    LogErr(ERROR_LEVEL, ER_CANT_CREATE_HANDLER_OBJECT_FOR_TABLE,
           schema_name.c_str(), table_name.c_str());
    return true;
  }
  table->file= file;
  table_guard.update_handler(file);

  if (table->file->set_ha_share_ref(&share.ha_share))
  {
    LogErr(ERROR_LEVEL, ER_CANT_SET_HANDLER_REFERENCE_FOR_TABLE,
           table_name.c_str(), schema_name.c_str());
    return true;
  }

  /*
    Fix pointers in TABLE, TABLE_SHARE and fields.
    These steps are necessary for correct handling of
    default values by Create_field constructor.
  */
  table->s->db_low_byte_first= table->file->low_byte_first();
  table->use_all_columns();
  table->record[0]= table->record[1]= share.default_values;
  table->null_row= 0;
  table->field= share.field;
  table->key_info= share.key_info;

  /*
    Storage engine finds the auto_increment column
    based on TABLE::found_next_number_field. auto_increment value is
    maintained by Storage Engine, and it is calculated dynamically
    every time SE opens the table. Without setting this value, SE will
    not set auto_increment value for the table.
  */
  if (share.found_next_number_field)
    table->found_next_number_field=
      table->field[(uint) (share.found_next_number_field - share.field)];

  //Set table_name variable and table in fields
  const char *alias= "";
  for (ptr= share.field ; (field= *ptr); ptr++)
  {
    field->table= table;
    field->table_name= &alias;
  }

  // Check presence of old data types
  bool avoid_temporal_upgrade_saved= avoid_temporal_upgrade;
  avoid_temporal_upgrade= false;
  error= check_table_for_old_types(table);
  avoid_temporal_upgrade= avoid_temporal_upgrade_saved;

  if (error)
  {
    LogErr(ERROR_LEVEL, ER_TABLE_NEEDS_UPGRADE, table_name.c_str());
    return true;
  }

  uint i= 0;
  KEY *key_info= share.key_info;

  /*
    Mark all the keys visible and supported algorithm explicit.
    Unsupported algorithms will get fixed by prepare_key() call.
  */
  key_info= share.key_info;
  for (i=0 ; i < share.keys ; i++,key_info++)
  {
    key_info->is_visible= true;
    /*
      Fulltext and Spatical indexes will get fixed by
      mysql_prepare_create_table()
    */
    if (key_info->algorithm != HA_KEY_ALG_SE_SPECIFIC &&
        !(key_info->flags & HA_FULLTEXT) &&
        !(key_info->flags & HA_SPATIAL) &&
        table->file->is_index_algorithm_supported(key_info->algorithm))
          key_info->is_algorithm_explicit= true;
  }

  // Fill create_info to be passed to the DD framework.
  HA_CREATE_INFO create_info;
  Alter_info alter_info(thd->mem_root);
  Alter_table_ctx alter_ctx;

  /*
    Replace thd->mem_root as prepare_fields_and_keys() and
    mysql_prepare_create_table() allocates memory in thd->mem_root.
  */
  MEM_ROOT *mem_root_backup= thd->mem_root;
  thd->mem_root= &table->mem_root;

  fill_create_info_for_upgrade(&create_info, table);

  if (prepare_fields_and_keys(thd, nullptr, table, &create_info,
                              &alter_info, &alter_ctx,
                              create_info.used_fields))
  {
    thd->mem_root= mem_root_backup;
    return true;
  }

  // Fix keys and indexes.
  KEY *key_info_buffer;
  uint key_count;

  // Foreign keys are handled at later stage by retrieving info from SE.
  FOREIGN_KEY *dummy_fk_key_info= NULL;
  uint fk_key_count= 0;

  if (mysql_prepare_create_table(thd, schema_name.c_str(), table_name.c_str(),
                                 &create_info, &alter_info,
                                 file, &key_info_buffer, &key_count,
                                 &dummy_fk_key_info, &fk_key_count,
                                 alter_ctx.fk_info, alter_ctx.fk_count,
                                 0, false /* No FKs here. */))
  {
    thd->mem_root= mem_root_backup;
    return true;
  }

  // Restore thd mem_root
  thd->mem_root= mem_root_backup;

  int select_field_pos= alter_info.create_list.elements;
  create_info.null_bits= 0;
  Create_field *sql_field;
  List_iterator<Create_field> it_create(alter_info.create_list);

  for (int field_no= 0; (sql_field=it_create++) ; field_no++)
  {
    if (prepare_create_field(thd, &create_info, &alter_info.create_list,
                             &select_field_pos, table->file, sql_field, field_no))
      return true;
  }

  // open_table_from_share and partition expression parsing needs a
  // valid SELECT_LEX to parse generated columns
  LEX *lex_saved= thd->lex;
  LEX lex;
  thd->lex= &lex;
  lex_start(thd);
  table_guard.update_lex(lex_saved);

  if (fill_partition_info_for_upgrade(thd, &share, &frm_context, table))
    return true;

  // Add name of all tablespaces used by partitions to the hash set.
  Tablespace_hash_set tablespace_name_set(PSI_INSTRUMENT_ME);
  if (thd->work_part_info != nullptr)
  {
    List_iterator<partition_element>
      partition_iter(thd->work_part_info->partitions);
    partition_element *partition_elem;

    while ((partition_elem= partition_iter++) )
    {
      if (partition_elem->tablespace_name != nullptr)
      {
        // Add name of all partitions to take MDL
        tablespace_name_set.insert(partition_elem->tablespace_name);
      }
      if (thd->work_part_info->is_sub_partitioned())
      {
        // Add name of all sub partitions to take MDL
        List_iterator<partition_element> sub_it(partition_elem->subpartitions);
        partition_element *sub_elem;
        while ((sub_elem= sub_it++))
        {
          if(sub_elem->tablespace_name != nullptr)
          {
            tablespace_name_set.insert(sub_elem->tablespace_name);
          }
        }
      }
    }
  }

  // Add name of the tablespace used by table to the hash set.
  if (share.tablespace != nullptr)
    tablespace_name_set.insert(share.tablespace);

  /*
    Acquire lock on tablespace names

    No lock is needed when creating DD objects from system thread
    handling server bootstrap/initialization.
    And in cases when lock is required it is X MDL and not IX lock
    the code acquires.

    However since IX locks on tablespaces used for table creation we
    still have to acquire locks. IX locks are acquired on tablespaces
    to satisfy asserts in dd::create_table()).
  */
  if ((tablespace_name_set.size() != 0) &&
    mdl_guard.acquire_lock_tablespace(&tablespace_name_set))
  {
    LogErr(ERROR_LEVEL, ER_CANT_LOCK_TABLESPACE, share.tablespace);
     return true;
  }


  /*
    Generated columns are fixed here as open_table_from_share()
    asserts that Field objects in TABLE_SHARE doesn't have
    expressions assigned.
  */
  if (fix_generated_columns_for_upgrade(thd, table, alter_info.create_list))
  {
    LogErr(ERROR_LEVEL,
           ER_CANT_UPGRADE_GENERATED_COLUMNS_TO_DD);
    return true;
  }

  FOREIGN_KEY *fk_key_info_buffer= NULL;
  uint fk_number= 0;

  // Set sql_mode=0 for handling default values, it will be restored vai RAII.
  thd->variables.sql_mode= 0;
  // Disable autocommit option in thd variable
  Disable_autocommit_guard autocommit_guard(thd);

  // Rename for stats tables before creating entry in dictionary
  bool is_innodb_stats_table= (schema_name == MYSQL_SCHEMA_NAME.str) &&
                              (table_name == table_stats ||
                               table_name == index_stats);

  String_type to_table_name(table_name);
  if (is_innodb_stats_table)
    to_table_name.append("_backup57");

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Schema *sch_obj= nullptr;

  if (thd->dd_client()->acquire(schema_name, &sch_obj))
  {
    // Error is reported by the dictionary subsystem.
    return true;
  }

  if (!sch_obj)
  {
    my_error(ER_BAD_DB_ERROR, MYF(0), schema_name.c_str());
    return true;
  }

  Disable_gtid_state_update_guard disabler(thd);

  if (dd::create_dd_user_table(thd,
                               *sch_obj,
                               to_table_name,
                               &create_info,
                               alter_info.create_list,
                               key_info_buffer,
                               key_count,
                               Alter_info::ENABLE,
                               fk_key_info_buffer,
                               fk_number,
                               table->file))
  {
    LogErr(ERROR_LEVEL, ER_DD_ERROR_CREATING_ENTRY,
           schema_name.c_str(), table_name.c_str());
    trans_rollback_stmt(thd);
    // Full rollback in case we have THD::transaction_rollback_request.
    trans_rollback(thd);
    return true;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd))
  {
    LogErr(ERROR_LEVEL, ER_DD_ERROR_CREATING_ENTRY,
           schema_name.c_str(), table_name.c_str());
    return true;
  }

  if (set_se_data_for_user_tables(thd, schema_name, to_table_name,
                                  table, is_innodb_stats_table))
  {
    LogErr(ERROR_LEVEL, ER_DD_CANT_FIX_SE_DATA,
           schema_name.c_str(), table_name.c_str());
    return true;
  }

  if (fix_fk_parent_key_names(thd, schema_name, to_table_name,
                              share.db_type()))
  {
    return true;
  }

  MEM_ROOT *thd_mem_root= thd->mem_root;
  thd->mem_root= &table->mem_root;
  error= add_triggers_to_table(thd, table, schema_name, table_name);
  thd->mem_root= thd_mem_root;

  return error;
}


/**
  Upgrade mysql.plugin table. This is required to initialize the plugins.
  User tables will be upgraded after all the plugins are initialized.
*/

bool migrate_plugin_table_to_dd(THD *thd)
{
  return migrate_table_to_dd(thd, "mysql", "plugin", false);
}


/**
  Scan the database to identify all .frm files.
  Triggers existence will be checked only for tables found here.
*/

bool migrate_all_frm_to_dd(THD *thd, const char *dbname,
                           bool is_fix_view_cols_and_deps)
{
  uint i;
  MY_DIR *a;
  String_type path;
  bool error= false;

  path.assign(mysql_real_data_home);
  path += dbname;

  if (!(a = my_dir(path.c_str(), MYF(MY_WANT_STAT))))
  {
    LogErr(ERROR_LEVEL, ER_CANT_OPEN_DIR, path.c_str());
    return true;
  }
  for (i = 0; i < (uint)a->number_off_files; i++)
  {
    String_type file;

    file.assign(a->dir_entry[i].name);
    if (file.at(0)  == '.')
        continue;

    if(!MY_S_ISDIR(a->dir_entry[i].mystat->st_mode))
    {
      String_type file_ext;
      char schema_name[NAME_LEN + 1];
      char table_name[NAME_LEN + 1];

      if (file.size() < 4)
        continue;

      file_ext.assign(file.c_str() + file.size() - 4);

      // Skip if it is not .frm file.
      if (file_ext.compare(reg_ext))
        continue;

      // Skip for temporary tables.
      if (is_prefix(file.c_str(), tmp_file_prefix))
        continue;

      // Get the name without the file extension.
      file.erase(file.size() - 4, 4);
      // Construct the schema name from its canonical format.
      filename_to_tablename(dbname, schema_name, sizeof(schema_name));
      filename_to_tablename(file.c_str(), table_name, sizeof(table_name));

      /*
        Skip mysql.plugin tables during upgrade of user and system tables as
        it has been upgraded already after creating DD tables.
      */

      bool is_skip_table= ((strcmp(schema_name, "mysql") == 0) &&
                           (strcmp(table_name, "plugin") == 0)) ||
                          strcmp(schema_name,
                                 PERFORMANCE_SCHEMA_DB_NAME.str) == 0;

      if (is_skip_table)
        continue;

      // Create an entry in the new DD.
      bool result= false;
      result= migrate_table_to_dd(thd, schema_name, table_name,
                                  is_fix_view_cols_and_deps);

      /*
        Set error status, but don't abort upgrade
        as we want to process all tables.
      */
      error|= result;

      /*
        Upgrade process does not stop immediately if it encounters any error.
        All the tables in the data directory are processed and all error are
        reported to user at once. Server code has many checks for error in DA.
        if thd->is_error() return true, atempt to upgrade all subsequent tables
        will fail and error log will report error false positives.
     */
      thd->clear_error();
    }
  }
  my_dirend(a);
  return error;
}

} // namespace upgrade
} // namespace dd
