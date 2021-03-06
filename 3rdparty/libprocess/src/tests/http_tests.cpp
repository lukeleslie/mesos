#include <arpa/inet.h>

#include <gmock/gmock.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <string>

#include <process/address.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/socket.hpp>

#include <stout/base64.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "encoder.hpp"

using namespace process;

using process::http::URL;

using process::network::Socket;

using std::string;

using testing::_;
using testing::Assign;
using testing::DoAll;
using testing::EndsWith;
using testing::Invoke;
using testing::Return;


class HttpProcess : public Process<HttpProcess>
{
public:
  HttpProcess() {}

  MOCK_METHOD1(body, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(pipe, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(get, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(post, Future<http::Response>(const http::Request&));

protected:
  virtual void initialize()
  {
    route("/auth", None(), &HttpProcess::auth);
    route("/body", None(), &HttpProcess::body);
    route("/pipe", None(), &HttpProcess::pipe);
    route("/get", None(), &HttpProcess::get);
    route("/post", None(), &HttpProcess::post);
  }

  Future<http::Response> auth(const http::Request& request)
  {
    string encodedAuth = base64::encode("testuser:testpass");
    Option<string> authHeader = request.headers.get("Authorization");
    if (!authHeader.isSome() || (authHeader.get() != "Basic " + encodedAuth)) {
      return http::Unauthorized("testrealm");
    }
    return http::OK();
  }
};


class Http
{
public:
  Http() : process(new HttpProcess())
  {
    spawn(process.get());
  }

  ~Http()
  {
    terminate(process.get());
    wait(process.get());
  }

  Owned<HttpProcess> process;
};


TEST(HTTP, Auth)
{
  Http http;

  // Test the case where there is no auth.
  Future<http::Response> noAuthFuture = http::get(http.process->self(), "auth");

  AWAIT_READY(noAuthFuture);
  EXPECT_EQ(http::statuses[401], noAuthFuture.get().status);
  ASSERT_SOME_EQ("Basic realm=\"testrealm\"",
                 noAuthFuture.get().headers.get("WWW-authenticate"));

  // Now test passing wrong auth header.
  hashmap<string, string> headers;
  headers["Authorization"] = "Basic " + base64::encode("testuser:wrongpass");

  Future<http::Response> wrongAuthFuture =
    http::get(http.process->self(), "auth", None(), headers);

  AWAIT_READY(wrongAuthFuture);
  EXPECT_EQ(http::statuses[401], wrongAuthFuture.get().status);
  ASSERT_SOME_EQ("Basic realm=\"testrealm\"",
                 wrongAuthFuture.get().headers.get("WWW-authenticate"));

  // Now test passing right auth header.
  headers["Authorization"] = "Basic " + base64::encode("testuser:testpass");

  Future<http::Response> rightAuthFuture =
    http::get(http.process->self(), "auth", None(), headers);

  AWAIT_READY(rightAuthFuture);
  EXPECT_EQ(http::statuses[200], rightAuthFuture.get().status);
}


TEST(HTTP, Endpoints)
{
  Http http;

  // First hit '/body' (using explicit sockets and HTTP/1.0).
  Try<Socket> create = Socket::create();
  ASSERT_SOME(create);

  Socket socket = create.get();

  AWAIT_READY(socket.connect(http.process->self().address));

  std::ostringstream out;
  out << "GET /" << http.process->self().id << "/body"
      << " HTTP/1.0\r\n"
      << "Connection: Keep-Alive\r\n"
      << "\r\n";

  const string data = out.str();

  EXPECT_CALL(*http.process, body(_))
    .WillOnce(Return(http::OK()));

  AWAIT_READY(socket.send(data));

  string response = "HTTP/1.1 200 OK";

  AWAIT_EXPECT_EQ(response, socket.recv(response.size()));

  // Now hit '/pipe' (by using http::get).
  http::Pipe pipe;
  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  Future<Nothing> request;
  EXPECT_CALL(*http.process, pipe(_))
    .WillOnce(DoAll(FutureSatisfy(&request),
                    Return(ok)));

  Future<http::Response> future = http::get(http.process->self(), "pipe");

  AWAIT_READY(request);

  // Write the response.
  http::Pipe::Writer writer = pipe.writer();
  EXPECT_TRUE(writer.write("Hello World\n"));
  EXPECT_TRUE(writer.close());

  AWAIT_READY(future);
  EXPECT_EQ(http::statuses[200], future.get().status);
  EXPECT_SOME_EQ("chunked", future.get().headers.get("Transfer-Encoding"));
  EXPECT_EQ("Hello World\n", future.get().body);
}


TEST(HTTP, PipeEOF)
{
  http::Pipe pipe;
  http::Pipe::Reader reader = pipe.reader();
  http::Pipe::Writer writer = pipe.writer();

  // A 'read' on an empty pipe should block.
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Writing an empty string should have no effect.
  EXPECT_TRUE(writer.write(""));
  EXPECT_TRUE(read.isPending());

  // After a 'write' the pending 'read' should complete.
  EXPECT_TRUE(writer.write("hello"));
  ASSERT_TRUE(read.isReady());
  EXPECT_EQ("hello", read.get());

  // After a 'write' a call to 'read' should be completed immediately.
  ASSERT_TRUE(writer.write("world"));

  read = reader.read();
  ASSERT_TRUE(read.isReady());
  EXPECT_EQ("world", read.get());

  // Close the write end of the pipe and ensure the remaining
  // data can be read.
  EXPECT_TRUE(writer.write("!"));
  EXPECT_TRUE(writer.close());
  AWAIT_EQ("!", reader.read());

  // End of file should be reached.
  AWAIT_EQ("", reader.read());
  AWAIT_EQ("", reader.read());

  // Writes to a pipe with the write end closed are ignored.
  EXPECT_FALSE(writer.write("!"));
  AWAIT_EQ("", reader.read());

  // The write end cannot be closed twice.
  EXPECT_FALSE(writer.close());

  // Close the read end, this should not notify the writer
  // since the write end was already closed.
  EXPECT_TRUE(reader.close());
  EXPECT_TRUE(writer.readerClosed().isPending());
}


TEST(HTTP, PipeFailure)
{
  http::Pipe pipe;
  http::Pipe::Reader reader = pipe.reader();
  http::Pipe::Writer writer = pipe.writer();

  // Fail the writer after writing some data.
  EXPECT_TRUE(writer.write("hello"));
  EXPECT_TRUE(writer.write("world"));

  EXPECT_TRUE(writer.fail("disconnected!"));

  // The reader should read the data, followed by the failure.
  AWAIT_EQ("hello", reader.read());
  AWAIT_EQ("world", reader.read());

  Future<string> read = reader.read();
  EXPECT_TRUE(read.isFailed());
  EXPECT_EQ("disconnected!", read.failure());

  // The writer cannot close or fail an already failed pipe.
  EXPECT_FALSE(writer.close());
  EXPECT_FALSE(writer.fail("not again"));

  // The writer shouldn't be notified of the reader closing,
  // since the writer had already failed.
  EXPECT_TRUE(reader.close());
  EXPECT_TRUE(writer.readerClosed().isPending());
}



TEST(HTTP, PipeReaderCloses)
{
  http::Pipe pipe;
  http::Pipe::Reader reader = pipe.reader();
  http::Pipe::Writer writer = pipe.writer();

  // If the read end of the pipe is closed,
  // it should discard any unread data.
  EXPECT_TRUE(writer.write("hello"));
  EXPECT_TRUE(writer.write("world"));

  // The writer should discover the closure.
  Future<Nothing> closed = writer.readerClosed();
  EXPECT_TRUE(reader.close());
  EXPECT_TRUE(closed.isReady());

  // The read end is closed, subsequent reads will fail.
  AWAIT_FAILED(reader.read());

  // The read end is closed, writes are ignored.
  EXPECT_FALSE(writer.write("!"));
  AWAIT_FAILED(reader.read());

  // The read end cannot be closed twice.
  EXPECT_FALSE(reader.close());

  // Close the write end.
  EXPECT_TRUE(writer.close());

  // Reads should fail since the read end is closed.
  AWAIT_FAILED(reader.read());
}


TEST(HTTP, Encode)
{
  string unencoded = "a$&+,/:;=?@ \"<>#%{}|\\^~[]`\x19\x80\xFF";
  unencoded += string("\x00", 1); // Add a null byte to the end.

  string encoded = http::encode(unencoded);

  EXPECT_EQ("a%24%26%2B%2C%2F%3A%3B%3D%3F%40%20%22%3C%3E%23"
            "%25%7B%7D%7C%5C%5E%7E%5B%5D%60%19%80%FF%00",
            encoded);

  EXPECT_SOME_EQ(unencoded, http::decode(encoded));

  encoded = "a%24%26%2B%2C%2F%3A%3B%3D%3F%40+%22%3C%3E%23"
            "%25%7B%7D%7C%5C%5E%7E%5B%5D%60%19%80%FF%00";
  EXPECT_SOME_EQ(unencoded, http::decode(encoded));

  EXPECT_ERROR(http::decode("%"));
  EXPECT_ERROR(http::decode("%1"));
  EXPECT_ERROR(http::decode("%;1"));
  EXPECT_ERROR(http::decode("%1;"));
}


TEST(HTTP, PathParse)
{
  const string pattern = "/books/{isbn}/chapters/{chapter}";

  Try<hashmap<string, string> > parse =
    http::path::parse(pattern, "/books/0304827484/chapters/3");

  ASSERT_SOME(parse);
  EXPECT_EQ(4, parse.get().size());
  EXPECT_SOME_EQ("books", parse.get().get("books"));
  EXPECT_SOME_EQ("0304827484", parse.get().get("isbn"));
  EXPECT_SOME_EQ("chapters", parse.get().get("chapters"));
  EXPECT_SOME_EQ("3", parse.get().get("chapter"));

  parse = http::path::parse(pattern, "/books/0304827484");

  ASSERT_SOME(parse);
  EXPECT_EQ(2, parse.get().size());
  EXPECT_SOME_EQ("books", parse.get().get("books"));
  EXPECT_SOME_EQ("0304827484", parse.get().get("isbn"));

  parse = http::path::parse(pattern, "/books/0304827484/chapters");

  ASSERT_SOME(parse);
  EXPECT_EQ(3, parse.get().size());
  EXPECT_SOME_EQ("books", parse.get().get("books"));
  EXPECT_SOME_EQ("0304827484", parse.get().get("isbn"));
  EXPECT_SOME_EQ("chapters", parse.get().get("chapters"));

  parse = http::path::parse(pattern, "/foo/0304827484/chapters");

  EXPECT_ERROR(parse);
  EXPECT_EQ("Expecting 'books' not 'foo'", parse.error());

  parse = http::path::parse(pattern, "/books/0304827484/bar");

  EXPECT_ERROR(parse);
  EXPECT_EQ("Expecting 'chapters' not 'bar'", parse.error());

  parse = http::path::parse(pattern, "/books/0304827484/chapters/3/foo/bar");

  EXPECT_ERROR(parse);
  EXPECT_EQ("Not expecting suffix 'foo/bar'", parse.error());
}


http::Response validateGetWithoutQuery(const http::Request& request)
{
  EXPECT_NE(process::address(), request.client);
  EXPECT_EQ("GET", request.method);
  EXPECT_THAT(request.path, EndsWith("get"));
  EXPECT_EQ("", request.body);
  EXPECT_EQ("", request.fragment);
  EXPECT_TRUE(request.query.empty());

  return http::OK();
}


http::Response validateGetWithQuery(const http::Request& request)
{
  EXPECT_NE(process::address(), request.client);
  EXPECT_EQ("GET", request.method);
  EXPECT_THAT(request.path, EndsWith("get"));
  EXPECT_EQ("", request.body);
  EXPECT_EQ("", request.fragment);
  EXPECT_EQ("bar", request.query.at("foo"));
  EXPECT_EQ(1, request.query.size());

  return http::OK();
}


TEST(HTTP, Get)
{
  Http http;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(Invoke(validateGetWithoutQuery));

  Future<http::Response> noQueryFuture = http::get(http.process->self(), "get");

  AWAIT_READY(noQueryFuture);
  ASSERT_EQ(http::statuses[200], noQueryFuture.get().status);

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(Invoke(validateGetWithQuery));

  Future<http::Response> queryFuture =
    http::get(http.process->self(), "get", "foo=bar");

  AWAIT_READY(queryFuture);
  ASSERT_EQ(http::statuses[200], queryFuture.get().status);
}


TEST(HTTP, StreamingGetComplete)
{
  Http http;

  http::Pipe pipe;
  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  EXPECT_CALL(*http.process, pipe(_))
    .WillOnce(Return(ok));

  Future<http::Response> response =
    http::streaming::get(http.process->self(), "pipe");

  // The response should be ready since the headers were sent.
  AWAIT_READY(response);

  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(http::Response::PIPE, response.get().type);
  ASSERT_SOME(response.get().reader);

  http::Pipe::Reader reader = response.get().reader.get();

  // There is no data to read yet.
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Stream data into the body and read it from the response.
  http::Pipe::Writer writer = pipe.writer();
  EXPECT_TRUE(writer.write("hello"));
  AWAIT_EQ("hello", read);

  EXPECT_TRUE(writer.write("goodbye"));
  AWAIT_EQ("goodbye", reader.read());

  // Complete the response.
  EXPECT_TRUE(writer.close());
  AWAIT_EQ("", reader.read()); // EOF.
}


TEST(HTTP, StreamingGetFailure)
{
  Http http;

  http::Pipe pipe;
  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  EXPECT_CALL(*http.process, pipe(_))
    .WillOnce(Return(ok));

  Future<http::Response> response =
    http::streaming::get(http.process->self(), "pipe");

  // The response should be ready since the headers were sent.
  AWAIT_READY(response);

  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(http::Response::PIPE, response.get().type);
  ASSERT_SOME(response.get().reader);

  http::Pipe::Reader reader = response.get().reader.get();

  // There is no data to read yet.
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Stream data into the body and read it from the response.
  http::Pipe::Writer writer = pipe.writer();
  EXPECT_TRUE(writer.write("hello"));
  AWAIT_EQ("hello", read);

  EXPECT_TRUE(writer.write("goodbye"));
  AWAIT_EQ("goodbye", reader.read());

  // Fail the response.
  EXPECT_TRUE(writer.fail("oops"));
  AWAIT_FAILED(reader.read());
}


http::Response validatePost(const http::Request& request)
{
  EXPECT_EQ("POST", request.method);
  EXPECT_THAT(request.path, EndsWith("post"));
  EXPECT_EQ("This is the payload.", request.body);
  EXPECT_EQ("", request.fragment);
  EXPECT_TRUE(request.query.empty());

  return http::OK();
}


TEST(HTTP, Post)
{
  Http http;

  // Test the case where there is a content type but no body.
  Future<http::Response> future =
    http::post(http.process->self(), "post", None(), None(), "text/plain");

  AWAIT_EXPECT_FAILED(future);

  EXPECT_CALL(*http.process, post(_))
    .WillOnce(Invoke(validatePost));

  future = http::post(
      http.process->self(),
      "post",
      None(),
      "This is the payload.",
      "text/plain");

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);

  // Now test passing headers instead.
  hashmap<string, string> headers;
  headers["Content-Type"] = "text/plain";

  EXPECT_CALL(*http.process, post(_))
    .WillOnce(Invoke(validatePost));

  future =
    http::post(http.process->self(), "post", headers, "This is the payload.");

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);
}


TEST(HTTP, QueryEncodeDecode)
{
  // If we use Type<a, b> directly inside a macro without surrounding
  // parenthesis the comma will be eaten by the macro rather than the
  // template. Typedef to avoid the problem.
  typedef hashmap<string, string> HashmapStringString;

  EXPECT_EQ("",
            http::query::encode(HashmapStringString({})));

  EXPECT_EQ("foo=bar",
            http::query::encode(HashmapStringString({{"foo", "bar"}})));

  EXPECT_EQ("c%7E%2Fasdf=%25asdf&a()=b%2520",
            http::query::encode(
                HashmapStringString({{"a()", "b%20"}, {"c~/asdf", "%asdf"}})));

  EXPECT_EQ("d",
            http::query::encode(HashmapStringString({{"d", ""}})));

  EXPECT_EQ("a%26b%3Dc=d%26e%3Dfg",
            http::query::encode(HashmapStringString({{"a&b=c", "d&e=fg"}})));

  // Explicitly not testing decoding failures.
  EXPECT_SOME_EQ(HashmapStringString(),
                 http::query::decode(""));

  EXPECT_SOME_EQ(HashmapStringString({{"foo", "bar"}}),
                 http::query::decode("foo=bar"));

  EXPECT_SOME_EQ(HashmapStringString({{"a()", "b%20"}, {"c~/asdf", "%asdf"}}),
                 http::query::decode("c%7E%2Fasdf=%25asdf&a()=b%2520"));

  EXPECT_SOME_EQ(HashmapStringString({{"d", ""}}),
                 http::query::decode("d"));

  EXPECT_SOME_EQ(HashmapStringString({{"a&b=c", "d&e=fg"}}),
                 http::query::decode("a%26b%3Dc=d%26e%3Dfg"));
}


TEST(HTTP, CaseInsensitiveHeaders)
{
  http::Request request;
  request.headers["Content-Length"] = "20";
  EXPECT_EQ("20", request.headers["Content-Length"]);
  EXPECT_EQ("20", request.headers["CONTENT-LENGTH"]);
  EXPECT_EQ("20", request.headers["content-length"]);

  request.headers["content-length"] = "30";
  EXPECT_EQ("30", request.headers["content-length"]);
  EXPECT_EQ("30", request.headers["Content-Length"]);
  EXPECT_EQ("30", request.headers["CONTENT-LENGTH"]);

  http::Response response;
  response.headers["Content-Type"] = "application/json";
  EXPECT_EQ("application/json", response.headers["Content-Type"]);
  EXPECT_EQ("application/json", response.headers["content-type"]);
  EXPECT_EQ("application/json", response.headers["CONTENT-TYPE"]);

  response.headers["content-type"] = "text/javascript";
  EXPECT_EQ("text/javascript", response.headers["content-type"]);
  EXPECT_EQ("text/javascript", response.headers["Content-Type"]);
  EXPECT_EQ("text/javascript", response.headers["CONTENT-TYPE"]);
}


// TODO(evelinad): Add URLTest for IPv6.
TEST(URLTest, Stringification)
{
  EXPECT_EQ("http://mesos.apache.org:80/",
            stringify(URL("http", "mesos.apache.org")));

  EXPECT_EQ("https://mesos.apache.org:8080/",
            stringify(URL("https", "mesos.apache.org", 8080)));

  Try<net::IP> ip = net::IP::parse("172.158.1.23", AF_INET);
  ASSERT_SOME(ip);

  EXPECT_EQ("http://172.158.1.23:8080/",
            stringify(URL("http", ip.get(), 8080)));

  EXPECT_EQ("http://172.158.1.23:80/path",
            stringify(URL("http", ip.get(), 80, "/path")));

  hashmap<string, string> query;
  query["foo"] = "bar";
  query["baz"] = "bam";

  EXPECT_EQ("http://172.158.1.23:80/?baz=bam&foo=bar",
            stringify(URL("http", ip.get(), 80, "/", query)));

  EXPECT_EQ("http://172.158.1.23:80/path?baz=bam&foo=bar",
            stringify(URL("http", ip.get(), 80, "/path", query)));

  EXPECT_EQ("http://172.158.1.23:80/?baz=bam&foo=bar#fragment",
            stringify(URL("http", ip.get(), 80, "/", query, "fragment")));

  EXPECT_EQ("http://172.158.1.23:80/path?baz=bam&foo=bar#fragment",
            stringify(URL("http", ip.get(), 80, "/path", query, "fragment")));
}
