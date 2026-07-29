#include "../includes/Dispatcher.hpp"
#include "../includes/Router.hpp"
#include "../includes/HttpStatus.hpp"
#include "../includes/DeleteHandler.hpp"
#include "../includes/GetHandler.hpp"
#include "../includes/PostHandler.hpp"
#include <string>
#include <vector>

HttpResponse Dispatcher::dispatch(HttpRequest& request, const ServerConfig& server)
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
    // A redirect_url with no redirect_code is a 500, not a guessed 301 -- a wrong
    // permanent redirect gets cached by browsers and cannot be taken back.
    if (!location->redirect_url.empty())
    {
        if (location->redirect_code == 0)
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

    return HttpStatus::make_response(501);
}
