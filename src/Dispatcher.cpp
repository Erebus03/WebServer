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
    const LocationConfig* location = Router::match(request.uri, server);
    if (!location)
        return HttpStatus::make_response(404);

    if (!location->redirect_url.empty())
    {
        const int code = location->redirect_code;
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
            return HttpStatus::make_response(500);

        if (!FileUtils::is_header_safe(location->redirect_url))
            return HttpStatus::make_response(500);
        HttpResponse response = HttpStatus::make_response(location->redirect_code);
        response.headers["Location"] = location->redirect_url;
        return response;
    }

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
    if (request.method == "HEAD")
        return GetHandler::handle(request, *location);
    if (request.method == "DELETE")
        return DeleteHandler::handle(request, *location);
    if (request.method == "POST")
        return PostHandler::handle(request, *location);

    return HttpStatus::make_response(501);
}

void Dispatcher::attach_error_body(const HttpRequest& request, HttpResponse& response,
                                   const ServerConfig& server)
{
    (void)request;

    if (response.status_code < 400)
        return;

    if (!response.body.empty())
        return;

    const std::map<int, std::string>::const_iterator pages = server.error_pages.find(response.status_code);
    if (pages != server.error_pages.end())
    {

        if (FileUtils::read_file(pages->second, response.body) && !response.body.empty())
        {
            response.headers["Content-Type"] = "text/html; charset=UTF-8";
            return;
        }
    }

    std::ostringstream page_body;
    page_body << "<html><head><title>" << response.status_code << " "
              << response.status_message << "</title></head><body><h1>"
              << response.status_code << " " << response.status_message
              << "</h1></body></html>";

    response.body = page_body.str();
    response.headers["Content-Type"] = "text/html; charset=UTF-8";
}

static bool headers_are_safe(const HttpResponse& response)
{
    for (std::map<std::string, std::string>::const_iterator it = response.headers.begin();
         it != response.headers.end(); ++it)
    {
        if (!FileUtils::is_header_safe(it->first) || !FileUtils::is_header_safe(it->second))
            return false;
    }
    return true;
}

HttpResponse Dispatcher::dispatch(const HttpRequest& request, const ServerConfig& server)
{
    HttpResponse response = produce_response(request, server);

    if (!headers_are_safe(response))
        response = HttpStatus::make_response(500);

    attach_error_body(request, response, server);
    return response;
}
