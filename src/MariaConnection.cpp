#include "pch.h"
#include "MariaConnection.h"
#include "MariaResult.h"

MariaConnection::MariaConnection() :
  pConn_(NULL),
  pCurrentResult_(NULL),
  transacting_(false)
{
  LOG_VERBOSE;
}

MariaConnection::~MariaConnection() {
  LOG_VERBOSE;

  if (is_connected()) {
    warning("call dbDisconnect() when finished working with a connection");
    disconnect();
  }
}

void MariaConnection::connect(const Nullable<std::string>& host, const Nullable<std::string>& user,
                              const Nullable<std::string>& password, const Nullable<std::string>& db,
                              unsigned int port, const Nullable<std::string>& unix_socket,
                              unsigned long client_flag, const Nullable<std::string>& groups,
                              const Nullable<std::string>& default_file,
                              const Nullable<std::string>& ssl_key, const Nullable<std::string>& ssl_cert,
                              const Nullable<std::string>& ssl_ca, const Nullable<std::string>& ssl_capath,
                              const Nullable<std::string>& ssl_cipher) {
  LOG_VERBOSE;

  this->pConn_ = mysql_init(NULL);
  // Enable LOCAL INFILE for fast data ingest
  mysql_options(this->pConn_, MYSQL_OPT_LOCAL_INFILE, 0);
  // Default to UTF-8
  mysql_options(this->pConn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!groups.isNull())
    mysql_options(this->pConn_, MYSQL_READ_DEFAULT_GROUP,
                  as<std::string>(groups).c_str());
  if (!default_file.isNull())
    mysql_options(this->pConn_, MYSQL_READ_DEFAULT_FILE,
                  as<std::string>(default_file).c_str());

  if (!ssl_key.isNull() || !ssl_cert.isNull() || !ssl_ca.isNull() ||
      !ssl_capath.isNull() || !ssl_cipher.isNull()) {
    mysql_ssl_set(
      this->pConn_,
      ssl_key.isNull() ? NULL : as<std::string>(ssl_key).c_str(),
      ssl_cert.isNull() ? NULL : as<std::string>(ssl_cert).c_str(),
      ssl_ca.isNull() ? NULL : as<std::string>(ssl_ca).c_str(),
      ssl_capath.isNull() ? NULL : as<std::string>(ssl_capath).c_str(),
      ssl_cipher.isNull() ? NULL : as<std::string>(ssl_cipher).c_str()
    );
  }

  LOG_VERBOSE;

  if (!mysql_real_connect(this->pConn_,
                          host.isNull() ? NULL : as<std::string>(host).c_str(),
                          user.isNull() ? NULL : as<std::string>(user).c_str(),
                          password.isNull() ? NULL : as<std::string>(password).c_str(),
                          db.isNull() ? NULL : as<std::string>(db).c_str(),
                          port,
                          unix_socket.isNull() ? NULL : as<std::string>(unix_socket).c_str(),
                          client_flag)) {
    std::string error = mysql_error(this->pConn_);
    mysql_close(this->pConn_);
    this->pConn_ = NULL;

    stop("Failed to connect: %s", error.c_str());
  }
}

void MariaConnection::disconnect() {
  if (!is_connected()) return;

  if (has_query()) {
    warning(
      "%s\n%s",
      "There is a result object still in use.",
      "The connection will be automatically released when it is closed"
    );
  }

  try {
    mysql_close(conn());
  } catch (...) {};

  pConn_ = NULL;
}

bool MariaConnection::is_connected() {
  return !!conn();
}

void MariaConnection::check_connection() {
  if (!is_connected()) {
    stop("Invalid or closed connection");
  }
}

List MariaConnection::connection_info() {
  return
    List::create(
      _["host"] = std::string(pConn_->host),
      _["user"] = std::string(pConn_->user),
      _["dbname"] = std::string(pConn_->db ? pConn_->db : ""),
      _["conType"] = std::string(mysql_get_host_info(pConn_)),
      _["serverVersion"] = std::string(mysql_get_server_info(pConn_)),
      _["protocolVersion"] = (int) mysql_get_proto_info(pConn_),
      _["threadId"] = (int) mysql_thread_id(pConn_),
      _["client"] = std::string(mysql_get_client_info())
    );
}

MYSQL* MariaConnection::conn() {
  return pConn_;
}

std::string MariaConnection::quote_string(const Rcpp::String& input) {
  if (input == NA_STRING)
    return "NULL";

  const char* input_cstr = input.get_cstring();
  size_t input_len = strlen(input_cstr);

  // Create buffer with enough room to escape every character
  std::string output = "'";
  output.resize(input_len * 2 + 3);

  size_t end = mysql_real_escape_string(pConn_, &output[1], input_cstr, input_len);

  output.resize(end + 1);
  output.append("'");

  return output;
}

void MariaConnection::set_current_result(MariaResult* pResult) {
  if (pResult == pCurrentResult_)
    return;

  if (pCurrentResult_ != NULL) {
    if (pResult != NULL)
      warning("Cancelling previous query");

    pCurrentResult_->close();
  }
  pCurrentResult_ = pResult;
}

bool MariaConnection::is_current_result(MariaResult* pResult) {
  return pCurrentResult_ == pResult;
}

bool MariaConnection::has_query() {
  return pCurrentResult_ != NULL;
}

bool MariaConnection::exec(std::string sql) {
  check_connection();

  set_current_result(NULL);

  if (mysql_real_query(pConn_, sql.data(), sql.size()) != 0)
    stop("Error executing query: %s", mysql_error(pConn_));

  MYSQL_RES* res = mysql_store_result(pConn_);
  if (res != NULL)
    mysql_free_result(res);

  return true;
}

void MariaConnection::begin_transaction() {
  if (is_transacting()) stop("Nested transactions not supported.");
  check_connection();

  transacting_ = true;
}

void MariaConnection::commit() {
  if (!is_transacting()) stop("Call dbBegin() to start a transaction.");
  check_connection();

  mysql_commit(conn());
  transacting_ = false;
}

void MariaConnection::rollback() {
  if (!is_transacting()) stop("Call dbBegin() to start a transaction.");
  check_connection();

  mysql_rollback(conn());
  transacting_ = false;
}

bool MariaConnection::is_transacting() const {
  return transacting_;
}

void MariaConnection::autocommit() {
  if (!is_transacting() && conn()) {
    mysql_commit(conn());
  }
}
