#pragma once

#include <pqxx/params>
#include <pqxx/pqxx>

#include <string_view>
#include <utility>

namespace database {
template <typename... Parameters>
pqxx::result execute(pqxx::transaction_base& transaction, std::string_view query,
                     Parameters&&... parameters) {
    return transaction.exec(
        query, pqxx::params{std::forward<Parameters>(parameters)...});
}

template <typename... Parameters>
pqxx::row execute_one(pqxx::transaction_base& transaction, std::string_view query,
                      Parameters&&... parameters) {
    return execute(transaction, query, std::forward<Parameters>(parameters)...).one_row();
}
}
