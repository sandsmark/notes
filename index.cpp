/*
 *  A simple FastCGI application example in C++.
 *
 *  $Id: echo-cpp.cpp,v 1.10 2002/02/25 00:46:17 robs Exp $
 *
 *  Copyright (c) 2001  Rob Saccoccio and Chelsea Networks
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 *  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 *  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
extern char ** environ;
#endif
#include <fcgio.h>

#include <pqxx/connection.hxx>
#include <pqxx/transaction.hxx>

using namespace std;
using namespace pqxx;

// Maximum number of bytes allowed to be read from stdin
static const unsigned long STDIN_MAX = 1000000;

static void penv(const char * const * envp)
{
    cout << "<PRE>\n";
    for ( ; *envp; ++envp)
    {
        cout << *envp << "\n";
    }
    cout << "</PRE>\n";
}

static long gstdin(FCGX_Request * request, char ** content)
{
    char * clenstr = FCGX_GetParam("CONTENT_LENGTH", request->envp);
    unsigned long clen = STDIN_MAX;

    if (clenstr)
    {
        clen = strtol(clenstr, &clenstr, 10);
        if (*clenstr)
        {
            cerr << "can't parse \"CONTENT_LENGTH="
                 << FCGX_GetParam("CONTENT_LENGTH", request->envp)
                 << "\"\n";
            clen = STDIN_MAX;
        }

        // *always* put a cap on the amount of data that will be read
        if (clen > STDIN_MAX) clen = STDIN_MAX;

        *content = new char[clen];

        cin.read(*content, clen);
        clen = cin.gcount();
    }
    else
    {
        // *never* read stdin when CONTENT_LENGTH is missing or unparsable
        *content = 0;
        clen = 0;
    }

    // Chew up any remaining stdin - this shouldn't be necessary
    // but is because mod_fastcgi doesn't handle it correctly.

    // ignore() doesn't set the eof bit in some versions of glibc++
    // so use gcount() instead of eof()...
    do cin.ignore(1024); while (cin.gcount() == 1024);

    return clen;
}

int main (void)
{
    int count = 0;
    long pid = getpid();

    streambuf * cin_streambuf  = cin.rdbuf();
    streambuf * cout_streambuf = cout.rdbuf();
    streambuf * cerr_streambuf = cerr.rdbuf();

    connection conn("host=127.0.0.1 dbname=notes user=notes password=too6Phae");
    conn.prepare("get_note", "SELECT title, content FROM notes WHERE id = $1")("integer", prepare::treat_direct);
    conn.prepare("set_note", "UPDATE notes SET title = $1, content = $2 WHERE id = $3")("varchar", prepare::treat_string)("varchar", prepare::treat_string)("integer", prepare::treat_direct);
    conn.prepare("add_note", "INSERT INTO notes (title, content) VALUES($1, $2)")("varchar", prepare::treat_string)("varchar", prepare::treat_string);

    FCGX_Request request;

    FCGX_Init();
    FCGX_InitRequest(&request, 0, 0);

    while (FCGX_Accept_r(&request) == 0)
    {
        // Set up a transaction for this request
        work trans(conn, "get_data");

        // Note that the default bufsize (0) will cause the use of iostream
        // methods that require positioning (such as peek(), seek(),
        // unget() and putback()) to fail (in favour of more efficient IO).
        fcgi_streambuf cin_fcgi_streambuf(request.in);
        fcgi_streambuf cout_fcgi_streambuf(request.out);
        fcgi_streambuf cerr_fcgi_streambuf(request.err);

        cin.rdbuf(&cin_fcgi_streambuf);
        cout.rdbuf(&cout_fcgi_streambuf);
        cerr.rdbuf(&cerr_fcgi_streambuf);

        // Although FastCGI supports writing before reading,
        // many http clients (browsers) don't support it (so
        // the connection deadlocks until a timeout expires!).
        char * cont;
        unsigned long clen = gstdin(&request, &cont);

        cout << "Content-type: text/html\r\n"
                "\r\n"
                "<title>notes</title>\n"
                "<h1>notes</h1>\n"
                "<small>PID: " << pid << "\n"
                "Request Number: " << ++count << "</small>\n";

        string page = string(FCGX_GetParam("QUERY_STRING", request.envp));
        string note = string(FCGX_GetParam("QUERY_STRING", request.envp));
        string content = cont;


        if (page.length() == 0) {
            result res = trans.exec("SELECT id, title, lastchanged FROM notes");
            cout << "<table><tr><th>title</th><th>last changed</th>\n";
            for (result::const_iterator i = res.begin(); i != res.end(); i++) {
                cout << "<tr>" 
                      << "<td><a href='index.fcgi?" << i[0].c_str() << "'>" << i[1].c_str() << "</td>"
                      << "<td>" << i[2].c_str() << "</td>"
                     << "</tr>";
            }
            cout << "</table>";
        } else {
            result res = trans.prepared("get_note")(page).exec();
            cout << "<h1>" << res.begin()[0] << "</h1>";
            cout << "<form action=test.fcgi method=post>";
            cout << "<textarea name=content>" << res.begin()[1] << "</textarea>";
            cout << "<input type=submit>";
            cout << "</form>";
        }
        cout << "</body></html>";

        // If the output streambufs had non-zero bufsizes and
        // were constructed outside of the accept loop (i.e.
        // their destructor won't be called here), they would
        // have to be flushed here.
    }

    cin.rdbuf(cin_streambuf);
    cout.rdbuf(cout_streambuf);
    cerr.rdbuf(cerr_streambuf);

    return 0;
}
