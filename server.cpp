// HILFREICHE DOKUMENTATION
// * HTTP Message:          https://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html
// * HTTP Request Message:  https://www.w3.org/Protocols/rfc2616/rfc2616-sec5.html
// * HTTP Response Message: https://www.w3.org/Protocols/rfc2616/rfc2616-sec6.html

#define __STDC_WANT_LIB_EXT1__ 1
#include "mime.hpp"

#include "ws.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// Konstanten
const char *const HttpStatusOk               = "200 OK";
const char *const HttpStatusMovedPermanently = "301 Moved Permanently";
const char *const HttpStatusNotFound         = "404 Not Found";
const char *const HttpStatusInternalError    = "500 Internal Server Error";

const char *const HttpHeaderContentType   = "Content-Type";
const char *const HttpHeaderContentLength = "Content-Length";
const char *const HttpHeaderLocation      = "Location";

const char *const Sentinel              = "<body>";
const char *InterestingFileExtensions[] = { ".html", ".ts", ".css" };

const char *const SassDockerContainerName = "livegate-sass-node";

// CLI Optionen
char ContentDir[PATH_MAX] = { "." };
int MaxDepth = -1;
unsigned short Port          = 42250;
unsigned short WebSocketPort = 42251;
enum { SassDisabled, SassEnabled, SassDocker } SassMode = SassDisabled;
bool RunSassInDocker         = false;
bool RequestLoggingEnabled   = false;
bool ResponseLoggingEnabled  = false;

int ServerFd = -1;
int ClientFd = -1;
ws_cli_conn_t *ClientWebSocketConn = NULL;
pid_t SassWatcherPid = -1;

const char Script[] = R"js(
<script type="text/javascript">
document.addEventListener("DOMContentLoaded", (event) => {
    var scrollOffset = localStorage.getItem("scrollOffset")
    if (scrollOffset != null) {
        console.log("Setze window.scrollY auf " + scrollOffset)
        window.scrollTo(0, scrollOffset)
    }

    let socket = new WebSocket(`ws://localhost:${(window.location.port|0) + 1}/netzsteckdose`)

    socket.onopen = function(event) {
        console.log("[onopen] Verbindung hergestellt; window.location.pathname " + window.location.pathname)
        socket.send(window.location.pathname)
    }

    socket.onmessage = function(event) {
        console.log("[onmessage] Nachricht empfangen: '" + event.data + "'; window.location.pathname '" + window.location.pathname + "'");

        var changedFilename = event.data
        var myFilename = window.location.pathname;
        var shouldReload =
            changedFilename === "*" ||
            myFilename === changedFilename ||
            myFilename === "" && changedFilename === "index.html" ||
            myFilename.endsWith("/") && changedFilename === myFilename + "index.html";

        if (shouldReload) {
            localStorage.setItem("scrollOffset", window.scrollY)
            location.reload(true);
        } else {
            console.log(`[onmessage]: Lade nicht neu, '${event.data}' ist nicht meine Datei`)
        }
    }

    socket.onclose = function(event) {
        if (event.wasClean) {
            console.log("[onclose] Socket sauber geschlossen")
        } else {
            console.log("[onclose] Socket unsauber geschlossen")
        }
    }

    socket.onerror = function(error) {
        console.log("[onerror] Socket Fehler: " + JSON.stringify(error))
    }
});
</script>
)js";

template<typename f> struct deferer
{
    f F;
    deferer(f F) : F(F) {}
    ~deferer() { F(); }
};

struct defer_dummy {};
template<typename f> deferer<f> operator+(defer_dummy, f &&F) { return deferer<f>{F}; }
#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define defer auto DEFER_2(ScopeExit, __LINE__) = defer_dummy{} + [&]()

struct request
{
    char Path[PATH_MAX];
    char ResolvedPath[PATH_MAX];
};

struct header
{
    char *Name;    // NOTE: free()
    char *Value;   // NOTE: free()
    header *Next;  // NOTE: free() rekursiv
};

struct response
{
    const char *Status;
    header     *FirstHeader;  // NOTE: free()
    char       *Content;      // NOTE: free()
    size_t      ContentSize;
};

void AddHeader(response *Response, const char *Name, const char *Format, ...)
{
    header *Header = (header *)malloc(sizeof(header));

    Header->Name  = strdup(Name);
    Header->Value = (char *)malloc(1024);
    Header->Next  = NULL;

    va_list VaList;
    va_start(VaList, Format);
    vsnprintf(Header->Value, 1024, Format, VaList);
    va_end(VaList);

    if (Response->FirstHeader == NULL)
    {
        Response->FirstHeader = Header;
    }
    else
    {
        header *Last = Response->FirstHeader;
        while (Last->Next != NULL) Last = Last->Next;
        Last->Next = Header;
    }
}

char *ReadEntireContentFile(const char *Filename, size_t *Size = NULL);
char *ReadEntireFile(const char *Path, size_t *Size = NULL);
void GetContentFilePath(const char *Filename, char Output[PATH_MAX]);

void PrintError(const char *Message, ...)
{
    char Format[1024];
    snprintf(Format, sizeof(Format), "FEHLER: %s (%s)\n", Message, strerror(errno));

    va_list VaList;
    va_start(VaList, Message);
    vfprintf(stderr, Format, VaList);
    va_end(VaList);
}

void RunCommand(const char *Format, ...)
{
    char Command[1024];
    va_list VaList;
    va_start(VaList, Format);
    vsnprintf(Command, sizeof(Command), Format, VaList);
    va_end(VaList);

    printf("Führe Befehl aus: %s\n", Command);
    system(Command);
}

//
// FileWatcher
//

struct file_watcher_entry
{
    int CTime;
    char *Path;
};

const char *GetFilenameExtension(const char *Filename)
{
    return strrchr(Filename, '.');
}

bool IsInterestingForWatcher(const char *Filename)
{
    const char *FilenameExtension = GetFilenameExtension(Filename);
    if (FilenameExtension == NULL)
    {
        return false;
    }

    for (int I = 0; I < ARRAY_LEN(InterestingFileExtensions); ++I)
    {
        const char *InterestingExtension = InterestingFileExtensions[I];
        if (strcmp(FilenameExtension, InterestingExtension) == 0)
        {
            return true;
        }
    }

    return false;
}

bool NotifyClientFileChanged(const char *Filename)
{
    if (ClientWebSocketConn == NULL)
    {
        PrintError("Es ist keine WebSocket-Verbindung offen, kann den Client nicht über die Änderung benachrichtigen.\n");
        return false;
    }

    ws_sendframe_txt(ClientWebSocketConn, Filename);
    return true;
}

bool WatcherWalkDir(const char *DirPath, file_watcher_entry *Entries, size_t NumEntries, int Depth = 0)
{
    //for (int I = 0; I < Depth; ++I) printf("....");
    //printf("Scanne Verzeichnis %s\n", DirPath);

    DIR *Dir = opendir(DirPath);
    if (Dir == NULL)
    {
        PrintError("Konnte das Verzeichnis '%s' nicht öffnen.", DirPath);
        return false;
    }

    defer { closedir(Dir); Dir = NULL; };


    for (dirent *Ent; (Ent = readdir(Dir));)
    {
        const char *Filename = Ent->d_name;
        if (strcmp(Filename, ".") == 0 || strcmp(Filename, "..") == 0)
        {
            continue;
        }

        char Path[PATH_MAX];
        snprintf(Path, sizeof(Path), "%s/%s", DirPath, Filename);
        realpath(Path, Path);
        char *RelativePath = Path + strlen(ContentDir);

        struct stat Stat;
        stat(Path, &Stat);
        if (S_ISDIR(Stat.st_mode))
        {
            if (MaxDepth == -1 || Depth < MaxDepth)
            {
                WatcherWalkDir(Path, Entries, NumEntries, Depth + 1);
            }
        }
        else
        {
            if (!IsInterestingForWatcher(Filename))
            {
                continue;
            }

            file_watcher_entry *FoundEntry = NULL;
            for (int I = 0; I < NumEntries; ++I)
            {
                file_watcher_entry *Entry = &Entries[I];
                if (Entry->Path == NULL) continue;

                if (strcmp(Entry->Path, Path) == 0)
                {
                    FoundEntry = Entry;
                    break;
                }
            }

            if (FoundEntry != NULL)
            {
                bool FileChanged = FoundEntry->CTime != Stat.st_ctime;
                if (FileChanged)
                {
                    printf("Datei %s geändert! %d vs %d\n", RelativePath, (int)Stat.st_ctime, (int)FoundEntry->CTime);
                    FoundEntry->CTime = Stat.st_ctime;

                    bool IsTypescript = strcmp(GetFilenameExtension(Filename), ".ts") == 0;
                    bool IsCss        = strcmp(GetFilenameExtension(Filename), ".css") == 0;
                    bool ShouldReloadAll = IsTypescript || IsCss;
                    if (IsTypescript)
                    {
                        printf("Kompiliere TypeScript...\n");
                        RunCommand("tsc");
                        printf("...fertig.\n");
                    }

                    NotifyClientFileChanged(ShouldReloadAll ? "*" : RelativePath);
                }
            }
            else
            {
                printf("Neue Datei %s!\n", Path);

                // Freien Slot finden und file_watcher_entry erstellen
                bool FreeSlotFound = false;
                for (int I = 0; I < NumEntries; ++I)
                {
                    file_watcher_entry *Entry = &Entries[I];

                    bool IsFree = Entry->Path == NULL;
                    if (IsFree)
                    {
                        FreeSlotFound = true;
                        Entry->Path  = strdup(Path);
                        Entry->CTime = Stat.st_ctime;
                        break;
                    }
                }

                if (!FreeSlotFound)
                {
                    PrintError("Konnte keinen freien Slot mehr finden - bitte Anzahl der allokierten Slots erhöhen oder die maximale Verzeichnistiefe reduzieren");
                    return false;
                }
            }
        }
    }

    // TODO Gelöschte Dateien entfernen

    return true;
}

void *FileWatcherThreadCallback(void *Arg)
{
    bool *IsRunning = (bool *)Arg;
    size_t NumEntries = 1000;
    file_watcher_entry *Entries = (file_watcher_entry *)malloc(sizeof(file_watcher_entry) * NumEntries);
    defer
    {
        for (int I = 0; I < NumEntries; ++I) free(Entries[I].Path);
        free(Entries); Entries = NULL;
    };

    printf("File-Watcher gestartet.\n");

    while (*IsRunning)
    {
        if (!WatcherWalkDir(ContentDir, Entries, NumEntries))
        {
            PrintError("Es gab einen Fehler beim Scannen der Verzeichnisstruktur.");
            return NULL;
        }

        usleep(50 * 1000);
    }

    printf("File-Watcher gestoppt.\n");

    return NULL;
}

//
// SASS-Watcher
//

void StartSassWatcher()
{
    if (SassWatcherPid != -1)
    {
        PrintError("SassWatcher bereits gestartet!");
        return;
    }

    if (SassMode == SassDisabled)
    {
        return;
    }

    printf("Starte den SASS-Watcher.\n");

    SassWatcherPid = fork();
    if (SassWatcherPid == 0)
    {
        // Child process

        const char *SassMapFilename = "sass-map.txt";
        char *SassMap = ReadEntireContentFile(SassMapFilename);
        if (SassMap == NULL)
        {
            SassMap = strdup("scss/:css/");
            printf("%s nicht gefunden, verwende Fallback-Map '%s'\n", SassMapFilename, SassMap);
        }

        defer { free(SassMap); SassMap = NULL; };

        if (SassMode == SassDocker)
        {
            char Pwd[PATH_MAX];
            getcwd(Pwd, sizeof(Pwd));

            RunCommand(
                "docker run --rm -d -it --name %s -v %s:/sass node bash -c 'npm install -g sass && cd /sass && sass --watch %s'",
                SassDockerContainerName,
                Pwd,
                SassMap);
            RunCommand(
                "docker logs --follow %s",
                SassDockerContainerName);
        }
        else
        {
            assert(SassMode == SassEnabled);
            RunCommand("sass --watch %s", SassMap);
        }
    }
    else if (SassWatcherPid > 0)
    {
        // Parent process
        return;
    }
    else
    {
        PrintError("SASS-Watcher: fork() fehlgeschlagen");
    }
}

//
// Hilfsfunktionen
//

void WriteEntireBuffer(int Fd, const char *Buffer, size_t BufferSize)
{
    int Position = 0;
    while (Position <= BufferSize)
    {
        int Written = write(Fd, &Buffer[Position], BufferSize - Position);
        if (Written == 0)
        {
            return;
        }

        Position += Written;
    }

    assert(Position == BufferSize);
}

void GetContentFilePath(const char *Filename, char Output[PATH_MAX])
{
    assert(ContentDir != NULL);
    snprintf(Output, PATH_MAX, "%s/%s", ContentDir, Filename);
}

char *ReadEntireContentFile(const char *Filename, size_t *Size)
{
    char Path[PATH_MAX];
    GetContentFilePath(Filename, Path);
    return ReadEntireFile(Path, Size);
}

char *ReadEntireFile(const char *Path, size_t *Size)
{
    FILE *File = fopen(Path, "r");
    if (File == NULL)
    {
        PrintError("ReadEntireFile: Konnte %s nicht öffnen", Path);
        return NULL;
    }

    fseek(File, 0, SEEK_END);
    size_t FileSize = ftell(File);
    fseek(File, 0, SEEK_SET);

    char *Buffer = (char *)malloc(FileSize);
    size_t FileBytesRead = fread(Buffer, 1, FileSize, File);
    if (FileBytesRead != FileSize)
    {
        // TODO Loop
        PrintError("ReadEntireFile: Es wurde nicht die ganze Datei gelesen...!?");
        free(Buffer);
        Buffer = NULL;
    }
    else if (Size != NULL)
    {
        *Size = FileSize;
    }

    fclose(File);

    return Buffer;
}

//
// Anfragen-Bearbeitung
//

enum ResolveRequestFilePathResult { RequestedFileNotFound, RequestedFileFound, RedirectToDirectory };
ResolveRequestFilePathResult ResolveRequestFilePath(const char RequestPath[PATH_MAX], char Output[PATH_MAX])
{
    struct stat Stat;
    char RelativePath[PATH_MAX];

    if (strlen(RequestPath) != 0)
    {
        strncpy(RelativePath, RequestPath, PATH_MAX);
    }
    else
    {
        // Wenn Pfad leer: index.html
        strncpy(RelativePath, "index.html", PATH_MAX);
    }

    char ContentFilePath[PATH_MAX];
    GetContentFilePath(RelativePath, ContentFilePath);

    if (stat(ContentFilePath, &Stat) != 0)
    {
        // Weder Datei noch Verzeichnis existiert
        return RequestedFileNotFound;
    }

    if (S_ISREG(Stat.st_mode))
    {
        // Datei gefunden
        strncpy(Output, ContentFilePath, PATH_MAX);
        return RequestedFileFound;
    }

    if (!S_ISDIR(Stat.st_mode))
    {
        // Könnte ein block device, FIFO, symlink, UNIX socket etc. sein
        return RequestedFileNotFound;
    }

    // Wenn Verzeichnis ohne '/': 301 zu <dir>/
    bool EndsWithSlash = ContentFilePath[strlen(ContentFilePath) - 1] == '/';
    if (!EndsWithSlash)
    {
        return RedirectToDirectory;
    }

    // <dir>/index.html
    snprintf(Output, PATH_MAX, "%s/%s", ContentFilePath, "index.html");

    bool FileExists = stat(Output, &Stat) == 0 && S_ISREG(Stat.st_mode);
    
    return FileExists ? RequestedFileFound : RequestedFileNotFound;
}

void HandleRequest(request *Request, response *Response)
{
    switch (ResolveRequestFilePath(Request->Path, Request->ResolvedPath))
    {
        case RequestedFileNotFound:
        {
            PrintError("HandleRequest: Dateipfad für '%s' konnte nicht aufgelöst werden", Request->Path);

            Response->Status = HttpStatusNotFound;
            AddHeader(Response, HttpHeaderContentType, "text/html");

            Response->Content     = strdup("Datei wurde nicht gefunden");
            Response->ContentSize = strlen(Response->Content);

            return;
        }

        case RedirectToDirectory:
        {
            char Location[PATH_MAX];
            snprintf(Location, PATH_MAX, "%s/", Request->Path);
            PrintError("HandleRequest: Leite '%s' weiter zu '%s'", Request->Path, Location);

            Response->Status = HttpStatusMovedPermanently;
            AddHeader(Response, HttpHeaderContentType, "text/html");
            AddHeader(Response, HttpHeaderLocation, Location);

            Response->Content     = strdup("");
            Response->ContentSize = strlen(Response->Content);

            return;
        }

        case RequestedFileFound:
            break;

        default: assert(!"Ungültiges Ergebnis");
    }

    // Angefragte Datei lesen

    size_t FileSize = 0;
    char *FileBuffer = ReadEntireFile(Request->ResolvedPath, &FileSize);
    defer { free(FileBuffer); FileBuffer = NULL; };

    if (FileBuffer == NULL)
    {
        PrintError("HandleRequest: Konnte die angeforderte Datei nicht lesen");

        Response->Status = HttpStatusInternalError;
        AddHeader(Response, HttpHeaderContentType, "text/html");

        Response->Content     = strdup("Datei konnte nicht gelesen werden");
        Response->ContentSize = strlen(Response->Content);

        return;
    }

    const char *ContentType = GetContentTypeForFilename(Request->ResolvedPath);

    bool ShouldInject = strcmp(GetFilenameExtension(Request->ResolvedPath), ".html") == 0;
    if (!ShouldInject)
    {
        Response->Status = HttpStatusOk;
        AddHeader(Response, HttpHeaderContentType, ContentType);

        Response->ContentSize = FileSize;
        Response->Content     = (char *)malloc(FileSize);
        memcpy(Response->Content, FileBuffer, FileSize);

        return;
    }

    // Skript injizieren

    const char *SentinelPosition = strstr(FileBuffer, Sentinel);
    if (SentinelPosition == NULL)
    {
        printf("Konnte %s nicht finden!\n", Sentinel);

        Response->Status = HttpStatusOk;
        AddHeader(Response, HttpHeaderContentType, ContentType);

        Response->ContentSize = FileSize;
        Response->Content     = (char *)malloc(FileSize);
        memcpy(Response->Content, FileBuffer, FileSize);

        return;
    }

    Response->Status      = HttpStatusOk;
    AddHeader(Response, HttpHeaderContentType, ContentType);

    Response->ContentSize = FileSize + sizeof(Script);
    Response->Content     = (char *)malloc(Response->ContentSize);

    //printf("<head> gefunden!\n");
    size_t HeadIndex = (SentinelPosition - FileBuffer) + strlen(Sentinel);
    size_t Pos = 0;

    // Bis zum HeadIndex die originale Datei...
    memcpy(&Response->Content[Pos], &FileBuffer[0], HeadIndex);
    Pos += HeadIndex;

    // ...dann das Skript...
    memcpy(&Response->Content[Pos], &Script[0], sizeof(Script));
    Pos += sizeof(Script);

    // ...dann den Rest der originalen Datei.
    memcpy(&Response->Content[Pos], &FileBuffer[HeadIndex], FileSize - HeadIndex);
}

void HandleClient()
{
    // Request parsen

    size_t RequestBufferSize = 8192;
    char  *RequestBuffer     = (char *)malloc(RequestBufferSize);
    defer { free(RequestBuffer); RequestBuffer = NULL; };

    // -1 Damit am Ende noch mindestens eine '\0' steht.
    // TODO Loop
    int RequestBytesRead = read(ClientFd, RequestBuffer, RequestBufferSize - 1);
    if (RequestBytesRead <= 0)
    {
        PrintError("HandleClient: read() Fehler");
        return;
    }

    if (RequestBytesRead == RequestBufferSize - 1)  // TODO
    {
        printf("HandleClient: Warnung - Der Request-Buffer ist voll. Es kann sein, dass die Anfrage abgeschnitten ist und deshalb unerwartetes Verhalten auftritt.\n");
    }

    // Path aus der Request Line parsen - (siehe W3 HTTP-Message Dokumentation)

    const char *At = RequestBuffer;
    while (*At && !isspace(*At)) ++At; // Bis zum ersten Space springen, dann sind ist der Cursor bei dem Request Path
    while (isspace(*At)) ++At; // Space überspringen - danach kommt der Path
    while (*At == '/') ++At; // Das erste '/' überspringen
    const char *RequestPathStart = At;
    while (*At && !isspace(*At)) ++At; // Bis zum nächsten Space springen, dann ist der Path zu Ende
    assert(At != RequestBuffer);

    request Request{};
    for (size_t I = 0; I < (At - RequestPathStart) && I < PATH_MAX; ++I)
    {
        Request.Path[I] = RequestPathStart[I];
    }

    if (RequestLoggingEnabled)
    {
        printf(
            "\nRequest: Path='%s'; ResolvedPath='%s'\n%s\n",
            Request.Path,
            Request.ResolvedPath,
            RequestBuffer);
    }

    response Response{};
    defer
    {
        for (header *Header = Response.FirstHeader; Header != NULL;)
        {
            free(Header->Name);
            free(Header->Value);
            header *Next = Header->Next;
            free(Header);
            Header = Next;
        }
        free(Response.Content);
    };

    HandleRequest(&Request, &Response);

    assert(Response.Status != NULL);
    assert(Response.Content != NULL);

    AddHeader(&Response, "Access-Control-Allow-Origin", "*");
    AddHeader(&Response, "Content-Length", "%d", (int)Response.ContentSize);

    // Response senden

    char H[4096];
    int I = 0;
    I += snprintf(&H[I], sizeof(H) - I, "HTTP/1.1 %s\r\n", Response.Status);
    for (header *Header = Response.FirstHeader; Header != NULL; Header = Header->Next)
    {
        I += snprintf(&H[I], sizeof(H) - I, "%s: %s\r\n", Header->Name, Header->Value);
    }
    I += snprintf(&H[I], sizeof(H) - I, "\r\n");

    WriteEntireBuffer(ClientFd, H, strlen(H));

    if (ResponseLoggingEnabled)
    {
        printf("\nResponse:\n%s\n", H);
    }

    if (Response.Content != NULL)
    {
        WriteEntireBuffer(ClientFd, Response.Content, Response.ContentSize);
    }
}

//
// WebSocket Handlers
//

void WebSocketOnOpen(ws_cli_conn_t *Conn)
{
    char *Client = ws_getaddress(Conn);
    printf("WebSocket Verbindung hergestellt: %s\n", Client);

    ws_close_client(ClientWebSocketConn);
    ClientWebSocketConn = Conn;
}

void WebSocketOnClose(ws_cli_conn_t *Conn)
{
    char *Client = ws_getaddress(Conn);
    printf("WebSocket Verbindung geschlossen: %s\n", Client);

    ws_close_client(ClientWebSocketConn);
    ClientWebSocketConn = NULL;
}

void WebSocketOnMessage(ws_cli_conn_t *Conn, const unsigned char *Message, uint64_t Size, int Type)
{
}

//
// Main
//

void PrintUsage()
{
    printf(
        "Usage: livegate\n"
        "    [--content-dir|-c CONTENT_DIR]\n"
        "    [--max-depth|-d MAX_DEPTH]\n"
        "    [--port|-p PORT]\n"
        "    [--sass|-s]\n"
        "    [--sass-docker]\n"
        "    [--log-requests|-q]\n"
        "    [--log-responses|-a]\n");
}

bool ParseArgs(int Argc, char **Argv)
{
    for (int I = 1; I < Argc; ++I)
    {
        const char *Arg = Argv[I];
        const char *NextArg = I == (Argc - 1) ? NULL : Argv[I + 1];

        if (strcmp(Arg, "--content-dir") == 0 || strcmp(Arg, "-c") == 0)
        {
            if (NextArg == NULL)
            {
                PrintUsage();
                return false;
            }

            strncpy(ContentDir, NextArg, sizeof(ContentDir));
            realpath(ContentDir, ContentDir);
            ++I;
            printf(" * Setze Inhalts-Verzeichnis = %s\n", ContentDir);
        }
        else if (strcmp(Arg, "--max-depth") == 0 || strcmp(Arg, "-d") == 0)
        {
            if (NextArg == NULL)
            {
                PrintUsage();
                return false;
            }

            char *EndPtr;
            MaxDepth = strtol(NextArg, &EndPtr, 10);
            ++I;
            if (EndPtr == NextArg)
            {
                PrintUsage();
                return false;
            }

            printf(" * Setze maximale Verzeichnistiefe = %d\n", MaxDepth);
        }
        else if (strcmp(Arg, "--port") == 0 || strcmp(Arg, "-p") == 0)
        {
            if (NextArg == NULL)
            {
                PrintUsage();
                return false;
            }

            char *EndPtr;
            Port = strtol(NextArg, &EndPtr, 10);
            ++I;
            if (EndPtr == NextArg)
            {
                PrintUsage();
                return false;
            }

            WebSocketPort = Port + 1;
            printf(" * Setze Port = %hu; WebSocket Port = %hu\n", Port, WebSocketPort);
        }
        else if (strcmp(Arg, "--sass") == 0)
        {
            SassMode = SassEnabled;
            printf(" * Führe den SASS Watcher aus\n");
        }
        else if (strcmp(Arg, "--sass-docker") == 0)
        {
            SassMode = SassDocker;
            printf(" * Führe den SASS Watcher in Docker aus\n");
        }
        else if (strcmp(Arg, "--log-requests") == 0 || strcmp(Arg, "-q") == 0)
        {
            RequestLoggingEnabled = true;
            printf(" * Anfrage-Logging aktiviert\n");
        }
        else if (strcmp(Arg, "--log-responses") == 0 || strcmp(Arg, "-a") == 0)
        {
            ResponseLoggingEnabled = true;
            printf(" * Antwort-Logging aktiviert\n");
        }
        else
        {
            PrintError("Unbekanntes Argument '%s'", Arg);
            PrintUsage();
            return false;
        }
    }

    return true;
}

void Shutdown()
{
    close(ServerFd); ServerFd = -1;
    close(ClientFd); ClientFd = -1;

    if (SassWatcherPid == -1)
    {
        return;
    }

    if (SassMode == SassDocker)
    {
        printf("Stoppe den Docker-Container...\n");
        RunCommand("docker rm -f %s", SassDockerContainerName);
        printf("...fertig.\n");
    }

    printf("Stoppe den SASS-Watcher Prozess...\n");
    kill(SassWatcherPid, SIGTERM);

    bool Died = false;
    for (int I = 0; I < 5; ++I)
    {
        int Status;
        if (waitpid(SassWatcherPid, &Status, WNOHANG) == SassWatcherPid)
        {
            Died = true;
            break;
        }

        printf("...warte auf den Tod... (%d/5)\n", I + 1);
        sleep(1);
    }

    if (!Died)
    {
        printf("SASS-Watcher ist hartnäckig.\n");
        kill(SassWatcherPid, SIGKILL);
    }

    printf("...fertig.\n");
}

int Run()
{
    // HTTP-Socket öffnen

    ServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (ServerFd == -1)
    {
        PrintError("Fehler beim Öffnen des Sockets");
        return 1;
    }

    int SoReuseAddr = 1;
    if (setsockopt(ServerFd, SOL_SOCKET, SO_REUSEADDR, &SoReuseAddr, sizeof(int)) < 0)
    {
        PrintError("Fehler beim Konfigurieren des Sockets");
        return 1;
    }

    sockaddr_in ServerAddress{};
    ServerAddress.sin_family      = AF_INET;
    ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    ServerAddress.sin_port        = htons(Port);

    int BindResult = bind(ServerFd, (sockaddr *)&ServerAddress, sizeof(ServerAddress));
    if (BindResult != 0)
    {
        PrintError("Fehler beim Binden des Sockets");
        return 1;
    }

    int ListenResult = listen(ServerFd, SOMAXCONN);
    if (ListenResult != 0)
    {
        PrintError("Fehler beim Binden des Sockets");
        return 1;
    }

    printf("LiveGate läuft auf Port=%hu, WebSocketPort=%hu\n", Port, WebSocketPort);

    // WebSocket öffnen

    struct ws_events WebSocketEvents;
    WebSocketEvents.onopen    = &WebSocketOnOpen;
    WebSocketEvents.onclose   = &WebSocketOnClose;
    WebSocketEvents.onmessage = &WebSocketOnMessage;
    int RunWebSocketOnOwnThread = 1;
    ws_socket(&WebSocketEvents, WebSocketPort, RunWebSocketOnOwnThread, 1000);

    StartSassWatcher();

    // Auf Verbindungen warten

    for (int ConnectionCounter = 0;; ++ConnectionCounter)
    {
        //printf("Warte auf Client\n");
        struct sockaddr_in ClientAddress;
        socklen_t Len = sizeof(ClientAddress);
        ClientFd = accept(ServerFd, (sockaddr *)&ClientAddress, &Len);
        if (ClientFd != -1)
        {
            //printf("[================CLIENT VERBUNDEN================] (%d)\n", ConnectionCounter);
            HandleClient();
            close(ClientFd); ClientFd = -1;
        }
        else
        {
            PrintError("Fehler beim Annehmen des Clients, weiter geht's");
        }
    }

    Shutdown();

    return 0;
}


void HandleSignal(int Signum)
{
    PrintError("Signal %d erhalten", Signum);
    Shutdown();
    exit(Signum);
}

int main(int Argc, char **Argv)
{
    signal(SIGINT, HandleSignal);
    signal(SIGKILL, HandleSignal);

    setvbuf(stdout, NULL, _IONBF, 0);

    int Result = 1;

    if (ParseArgs(Argc, Argv))
    {
        bool IsRunning = true;
        pthread_t WatcherThreadId;
        pthread_create(&WatcherThreadId, NULL, FileWatcherThreadCallback, &IsRunning);

        Result = Run();

        IsRunning = false;
        void *JoinStatus;
        pthread_join(WatcherThreadId, &JoinStatus);

        Shutdown();
    }

    printf("Auf Wiedersehen.\n");

    return Result;
}

