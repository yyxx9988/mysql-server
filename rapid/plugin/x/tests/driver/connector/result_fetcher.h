/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef X_TESTS_DRIVER_CONNECTOR_RESULT_FETCHER_H_
#define X_TESTS_DRIVER_CONNECTOR_RESULT_FETCHER_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mysqlxclient/xquery_result.h"
#include "mysqlxclient/xrow.h"


class Result_fetcher {
 public:
  using XQuery_result_ptr = std::unique_ptr<xcl::XQuery_result>;

  class Warning {
   public:
    Warning(
        const std::string &text,
        const uint32_t code,
        const bool is_note)
    : m_text(text), m_code(code), m_is_note(is_note) {
    }

    std::string m_text;
    uint32_t m_code;
    bool m_is_note;
  };

 public:
  explicit Result_fetcher(XQuery_result_ptr query)
  : m_query(std::move(query)) {
  }

  std::vector<xcl::Column_metadata> column_metadata() {
    if (m_error)
      return {};

    return m_query->get_metadata(&m_error);
  }

  const xcl::XRow* next() {
    if (m_cached_row) {
      auto result = m_cached_row;

      m_cached_row = nullptr;

      return result;
    }

    if (m_error)
      return nullptr;

    return m_query->get_next_row(&m_error);
  }

  bool next_data_set() {
    /* Skip empty resultsets */
    while (m_query->next_resultset(&m_error)) {
      m_cached_row = m_query->get_next_row(&m_error);

      if (nullptr != m_cached_row)
        return true;
    }

    return false;
  }

  xcl::XError get_last_error() const {
    return m_error;
  }

  int64_t last_insert_id() const {
    uint64_t result;
    if (!m_query->try_get_last_insert_id(&result))
      return -1;

    return result;
  }

  int64_t affected_rows() const {
    uint64_t result;
    if (!m_query->try_get_affected_rows(&result))
      return -1;

    return result;
  }

  std::string info_message() const {
    std::string result;

    m_query->try_get_info_message(&result);

    return result;
  }

  const std::vector<Warning> get_warnings() const {
    std::vector<Warning> result;

    for (const auto &warning : m_query->get_warnings()) {
      result.emplace_back(
          warning.msg(),
          warning.code(),
          warning.level() == ::Mysqlx::Notice::Warning_Level_NOTE);
    }

    return result;
  }

 private:
  XQuery_result_ptr m_query;
  xcl::XError       m_error;
  const xcl::XRow  *m_cached_row{ nullptr };
};

std::ostream& operator<<(
    std::ostream& os,
    const std::vector<xcl::Column_metadata>& meta);

std::ostream& operator<<(
    std::ostream& os,
    Result_fetcher* result);

#endif  // X_TESTS_DRIVER_CONNECTOR_RESULT_FETCHER_H_
