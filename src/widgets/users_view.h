/*
SCOPO: pannello "Users & permissions" (come la scheda Account utente di phpMyAdmin): utenti
       del server con i loro permessi in una matrice database x privilegio; ogni modifica è
       un'istruzione SQL (CREATE USER, GRANT, REVOKE, ...) che la finestra esegue e il pannello
       mostra. La GUI lavora sempre come amministratore; gli utenti valgono per il server API.
*/
#ifndef USERS_VIEW_H
#define USERS_VIEW_H

#include "../platform/platform.h"

typedef enum { UV_NONE = 0, UV_REDRAW, UV_CLOSE, UV_EXEC } UvAction;

typedef struct UsersView UsersView;

UsersView*  uv_create(PlatformWindow* win);
void        uv_destroy(UsersView* v);
void        uv_open(UsersView* v);                 // legge utenti, database e permessi (motore libero)
bool        uv_is_open(const UsersView* v);
void        uv_close(UsersView* v);
void        uv_draw(UsersView* v, int w, int h);
UvAction    uv_event(UsersView* v, sEvent* evt);
bool        uv_tick(UsersView* v, uint64_t now_ms);
const char* uv_sql(const UsersView* v);            // SQL da eseguire (UV_EXEC), statement separati da ";\n"
void        uv_exec_done(UsersView* v, bool ok, const char* message);   // esito; rilegge lo stato

#endif
