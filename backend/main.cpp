#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <iostream>
#include <string>
#include <random>
#include <memory>
#include <regex>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

const std::string SHORT_DOMAIN = "localhost:5000";
const int SHORT_CODE_LENGTH = 7;
const int SERVER_PORT = 8080;

const std::string RUNNING_MESSAGE = "URL Shortener Service is running!\n\n"
                               "Usage:\n"
                               "  POST/GET /makeshort/<url>  - Shorten a URL\n"
                               "  GET /<code> - Decode a short URL";

class CodeGenerator {
public:
    static std::string generate(int length = SHORT_CODE_LENGTH) {
        static const std::string chars =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789";

        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dist(0, chars. size() - 1);

        std::string code;
        code.reserve(length);
        for (int i = 0; i < length; ++i) {
            code += chars[dist(gen)];
        }
        return code;
    }
};

class Database {
public:
    Database(const std::string& db_path = "urls.db")
        : db_(db_path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
        initializeSchema();
    }

    void initializeSchema() {
        db_.exec(R"(
            CREATE TABLE IF NOT EXISTS urls (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                short_code TEXT UNIQUE NOT NULL,
                original_url TEXT NOT NULL,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            )
        )");
        db_.exec("CREATE INDEX IF NOT EXISTS idx_short_code ON urls(short_code)");
        db_.exec("CREATE INDEX IF NOT EXISTS idx_original_url ON urls(original_url)");
    }

    std::string shortenUrl(const std::string& original_url) {
        SQLite::Statement query(db_, "SELECT short_code FROM urls WHERE original_url = ? ");
        query.bind(1, original_url);
        if (query. executeStep()) {
            return query.getColumn(0). getText();
        }

        std::string short_code;
        int max_attempts = 10;
        for (int i = 0; i < max_attempts; ++i) {
            short_code = CodeGenerator::generate();
            try {
                SQLite::Statement insert(db_,
                    "INSERT INTO urls (short_code, original_url) VALUES (?, ?)");
                insert.bind(1, short_code);
                insert.bind(2, original_url);
                insert. exec();
                return short_code;
            } catch (const SQLite::Exception& e) {
                if (i == max_attempts - 1) throw;
            }
        }
        throw std::runtime_error("Failed to generate unique short code");
    }

    std::string getOriginalUrl(const std::string& short_code) {
        SQLite::Statement query(db_, "SELECT original_url FROM urls WHERE short_code = ? ");
        query. bind(1, short_code);
        if (query.executeStep()) {
            return query.getColumn(0).getText();
        }
        return "";
    }

private:
    SQLite::Database db_;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<Database> db)
        : socket_(std::move(socket)), db_(db) {}

    void start() {
        readRequest();
    }

private:
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    std::shared_ptr<Database> db_;

    void readRequest() {
        auto self = shared_from_this();
        http::async_read(socket_, buffer_, request_,
            [self](beast::error_code ec, std::size_t) {
                if (! ec) {
                    self->processRequest();
                }
            });
    }

    void processRequest() {
        std::string target(request_.target());
        std::string response_body;
        http::status status = http::status::ok;

        try {
            std::regex shorten_regex(R"(^/makeshort/(.+)$)");
            std::regex decode_regex(R"(/([a-zA-Z0-9]+)$)");
            std::smatch match;

            if (std::regex_match(target, match, shorten_regex)) {
                std::string original_url = match[1].str();

                original_url = urlDecode(original_url);

                std::string short_code = db_->shortenUrl(original_url);
                response_body = SHORT_DOMAIN + "/" + short_code;

            } else if (std::regex_match(target, match, decode_regex)) {
                std::string short_code = match[1].str();
                std::string original_url = db_->getOriginalUrl(short_code);

                if (original_url.empty()) {
                    status = http::status::not_found;
                    response_body = "Short URL not found";
                } else {
                    response_body = original_url;
                }

            } else if (target == "/" || target == "/health") {
                response_body = RUNNING_MESSAGE;

            } else {
                status = http::status::bad_request;
                response_body = "Invalid request.  Use /makeshort/<url> or /<code>";
            }

        } catch (const std::exception& e) {
            status = http::status::internal_server_error;
            response_body = std::string("Error: ") + e.what();
        }

        sendResponse(status, response_body);
    }

    std::string urlDecode(const std::string& encoded) {
        std::string decoded;
        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%' && i + 2 < encoded.size()) {
                int value;
                std::istringstream iss(encoded.substr(i + 1, 2));
                if (iss >> std::hex >> value) {
                    decoded += static_cast<char>(value);
                    i += 2;
                } else {
                    decoded += encoded[i];
                }
            } else if (encoded[i] == '+') {
                decoded += ' ';
            } else {
                decoded += encoded[i];
            }
        }
        return decoded;
    }

    void sendResponse(http::status status, const std::string& body) {
        auto response = std::make_shared<http::response<http::string_body>>(status, request_. version());
        response->set(http::field::server, "URLShortener/1.0");
        response->set(http::field::content_type, "text/plain");
        response->keep_alive(request_.keep_alive());
        response->body() = body;
        response->prepare_payload();

        auto self = shared_from_this();
        http::async_write(socket_, *response,
            [self, response](beast::error_code ec, std::size_t) {
                self->socket_.shutdown(tcp::socket::shutdown_send, ec);
            });
    }
};

class Server {
public:
    Server(net::io_context& ioc, unsigned short port)
        : ioc_(ioc)
        , acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
        , db_(std::make_shared<Database>()) {
        doAccept();
    }

private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<Database> db_;

    void doAccept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), db_)->start();
                }
                doAccept();
            });
    }
};

int main() {
    try {
        std::cout << "=== URL Shortener Service ===" << std::endl;
        std::cout << "Starting server on port " << SERVER_PORT << "..." << std::endl;

        net::io_context ioc{1};
        Server server(ioc, SERVER_PORT);

        std::cout << RUNNING_MESSAGE << std::endl;

        ioc.run();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}