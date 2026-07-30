#include "../includes/Dispatcher.hpp"
#include "../includes/FileUtils.hpp"
#include "../includes/Router.hpp"
#include "../includes/HttpStatus.hpp"
#include "../includes/DeleteHandler.hpp"
#include "../includes/GetHandler.hpp"
#include "../includes/PostHandler.hpp"
#include <string>
#include <vector>
#include <sstream>


HttpResponse Dispatcher::produce_response(const HttpRequest& request, const ServerConfig& server)
{
    // No matching location is a 404 rather than a synthesized default: building a
    // fallback location out of ServerConfig is config logic and belongs in the
    // parser. Consequence accepted -- every config must carry an explicit root
    // location, or it serves nothing.
    const LocationConfig* location = Router::match(request.uri, server);
    if (!location)
        return HttpStatus::make_response(404);

    // Redirects are answered before the method gate: the resource has moved, so
    // whether the method is permitted is the target's business, not ours.
    //
    // The code is checked against the set of real redirect codes rather than
    // merely against 0. The config parser converts whatever the file said with no
    // validation, so any integer can arrive here -- and an uninitialised
    // redirect_code can hold stack garbage that is non-zero, which a == 0 test
    // waves straight through onto the status line. Refusing loudly beats both
    // emitting a malformed status line and guessing 301, which browsers cache
    // permanently and which cannot then be taken back.
    if (!location->redirect_url.empty())
    {
        const int code = location->redirect_code;
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
            return HttpStatus::make_response(500);
        HttpResponse response = HttpStatus::make_response(location->redirect_code);
        response.headers["Location"] = location->redirect_url;
        return response;
    }

    // An empty methods vector means the author set no restriction, so everything
    // implemented is allowed -- not that this location refuses every request,
    // which is a location nobody could ever use.
    if (!location->methods.empty())
    {
        bool found = false;
        for (std::vector<std::string>::const_iterator it = location->methods.begin(); it != location->methods.end(); ++it)
        {
            if (*it == request.method)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            // Allow reflects what the config permits, not what this server
            // implements. While PostHandler is a stub, a config listing POST will
            // advertise it here and then answer 501 -- self-resolving, but worth
            // knowing before a demo.
            std::string results;
            for (std::vector<std::string>::const_iterator it = location->methods.begin(); it != location->methods.end(); ++it)
            {
                if (!results.empty())
                    results += ", ";
                results += *it;
            }

            HttpResponse response = HttpStatus::make_response(405);
            response.headers["Allow"] = results;
            return response;
        }
    }
    if (request.method == "GET")
        return GetHandler::handle(request, *location);
    else if (request.method == "DELETE")
        return DeleteHandler::handle(request, *location);
    else if (request.method == "POST")
        return PostHandler::handle(request, *location);

    // 501, not 405: the request reached here having passed the method gate, by
    // one of two routes -- the config listed this method explicitly, or the
    // config restricted nothing at all. Either way the client did what the
    // configuration invited and the gap is on the server's side, so blaming the
    // client with a 405 would be wrong.
    return HttpStatus::make_response(501);
}

void Dispatcher::attach_error_body(HttpResponse& response, const ServerConfig& server)
{
    if (response.status_code < 400)
        return;

    if (!response.body.empty())
        return;

    std::map<int, std::string>::const_iterator pages = server.error_pages.find(response.status_code);
    if (pages != server.error_pages.end())
    {
        // Both halves matter. read_file() succeeding is not enough: on Linux an
        // ifstream opens a directory and reads zero bytes, returning success with
        // an empty body, and a configured page that is simply an empty file does
        // the same. Either would ship a 404 with no body at all, which is exactly
        // what the fallback exists to prevent.
        if (FileUtils::read_file(pages->second, response.body) && !response.body.empty())
        {
            response.headers["Content-Type"] = "text/html; charset=UTF-8";
            return;
        }
    }
    // The floor of the fallback chain: generating a string in memory has no
    // failure mode. That is what guarantees termination -- if this step could
    // fail it would need an error page of its own, which could also fail, and
    // the error path would recurse until the stack gives out. Nothing here may
    // read the filesystem or the config.
    std::ostringstream page_body;
    page_body << "<html><head><title>" << response.status_code << " "
              << response.status_message << "</title></head><body><h1>"
              << response.status_code << " " << response.status_message
              << "</h1></body></html>";

    response.body = page_body.str();
    response.headers["Content-Type"] = "text/html; charset=UTF-8";
}

HttpResponse Dispatcher::dispatch(const HttpRequest& request, const ServerConfig& server)
{
    HttpResponse response = produce_response(request, server);
    attach_error_body(response, server);
    return response;
}
