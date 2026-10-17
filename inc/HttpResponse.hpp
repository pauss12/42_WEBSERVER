#ifndef HTTPRESPONSE_HPP
# define HTTPRESPONSE_HPP

#include <string>
#include <map>
#include <iostream>

# define ERROR_404 "File not found. It probably got swallowed by a tragic rm -rf * incident."
# define ERROR_405 "That verb isn't on the guest list for this endpoint. Strictly VIP access only."
# define ERROR_403 "Nice try, but not even chmod 777 can get you past this door."
# define ERROR_400 "Server: 'Was that HTTP or did you just drop your keyboard on the floor?'"
# define ERROR_413 "Your request body is too heavy for my buffer. Time to put that data on a diet."
# define ERROR_500 "Something exploded in the code. Don't panic—just pretend it was a glitch in the Matrix."
# define ERROR_501 "Feature coming soon! Check back in v2.0 after the developer gets some sleep."
# define ERROR_505 "Your HTTP version is so last season. Upgrade to the latest and greatest, please."
# define ERROR_408 "You started a sentence and never finished it. I got tired of waiting."
# define ERROR_431 "Those headers are longer than the terms and conditions nobody reads."
# define ERROR_409 "A file with that name already lives here. Great minds upload alike."
# define ERROR_502 "The script I asked for help just stared back at me silently. Awkward."
# define ERROR_504 "The script took a coffee break and never came back."
# define ERROR_414 "Your URL is longer than a Tolstoy novel. Please trim it down to size."

class HttpResponse
{
    private:
        int _status_code;
        std::string _status_message;
        std::map<std::string, std::string> _headers;
        std::string _body;
        std::string _start_line;
        // Detalle para el log del servidor (motivo de un error, destino de un
        // redirect...). No se envía al cliente.
        std::string _note;
        
        // Helper to convert int to string (mimicking your ToString)
        std::string toString(int value) const;

    public:
        HttpResponse();
        HttpResponse(const HttpResponse& other);
        HttpResponse&  operator=(const HttpResponse& other);
        ~HttpResponse();

        // GETTERS
        int getStatusCode() const;
        const std::string& getStatusMessage() const;
        const std::string& getBody() const;
        const std::map<std::string, std::string>& getHeaders() const;
        const std::string& getNote() const;

        // SETTERS
        void setStatusCode(int code);
        void setStatusMessage(const std::string& message);
        void setHeader(const std::string& key, const std::string& value);
        void setBody(const std::string& body);
        // Pone `body` como body de la respuesta sin copiarlo (swap)
        void swapBody(std::string& body);
        void setStartLine(const std::string& start_line);
        void setNote(const std::string& note);

        // Reason phrase estándar de un código (RFC 9110): 404 -> "Not Found"
        static std::string reasonPhrase(int code);

        // The "glue" method for the server
        std::string serialize() const;
};

#endif // HTTPRESPONSE_HPP