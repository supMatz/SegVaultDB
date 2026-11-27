/*
SCOPO: file e cartelle del motore leggibili solo dal proprietario anche su Windows (P2.6).
       Su POSIX bastano i permessi 0600 / 0700 (platform_compat.h); su Windows un file nuovo eredita l'ACL della
       cartella, che di solito lascia leggere tutti gli utenti del PC (gli hash delle password, i dati).
       sv_win_private sostituisce l'ACL con una protetta (niente eredità dalla cartella sopra): controllo
       completo all'utente che fa girare SegVault e a SYSTEM; per una cartella le voci si ereditano su ciò che
       ci verrà creato dentro.
*/
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <string.h>

int sv_win_private(const char* path, int is_dir) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return -1;
    BYTE buf[256];
    DWORD n = 0;
    BOOL got = GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n);
    CloseHandle(tok);
    if (!got) return -1;
    char* sid = NULL;
    if (!ConvertSidToStringSidA(((TOKEN_USER*)buf)->User.Sid, &sid)) return -1;
    char sddl[400];
    const char* inh = is_dir ? "OICI" : "";
    snprintf(sddl, sizeof(sddl), "D:P(A;%s;FA;;;%s)(A;%s;FA;;;SY)", inh, sid, inh);
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL)) return -1;
    BOOL present = FALSE, def = FALSE;
    PACL dacl = NULL;
    int rc = -1;
    if (GetSecurityDescriptorDacl(sd, &present, &dacl, &def) && present &&
        SetNamedSecurityInfoA((char*)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              NULL, NULL, dacl, NULL) == ERROR_SUCCESS) rc = 0;
    LocalFree(sd);
    return rc;
}

// ACL di path in forma SDDL ("D:P(A;;FA;;;S-1-5-...)(A;;FA;;;SY)"), "" se non si legge: per i test
void sv_win_acl_sddl(const char* path, char* out, size_t n) {
    out[0] = '\0';
    PSECURITY_DESCRIPTOR sd = NULL;
    char* s = NULL;
    if (GetNamedSecurityInfoA(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL, &sd) != ERROR_SUCCESS) return;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &s, NULL)) {
        snprintf(out, n, "%s", s);
        LocalFree(s);
    }
    LocalFree(sd);
}

// proprietario di path tra quelli fidati: Amministratori, SYSTEM, TrustedInstaller o l'utente che fa girare SegVault
static int owner_trusted(const char* path, PSID me) {
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID owner = NULL;
    if (GetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) != ERROR_SUCCESS)
        return 0;
    int ok = 0;
    if (owner && IsValidSid(owner)) {
        if (me && EqualSid(owner, me)) ok = 1;
        else if (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid)) ok = 1;
        else {
            char* s = NULL;
            if (ConvertSidToStringSidA(owner, &s)) {     // NT SERVICE\TrustedInstaller
                ok = !strcmp(s, "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
                LocalFree(s);
            }
        }
    }
    LocalFree(sd);
    return ok;
}

// CR-M8: un eseguibile o una DLL trovati in un percorso "noto" (C:\php, C:\xampp, una voce del PATH) si usano solo
// se il file e ogni cartella sopra di lui appartengono a un amministratore, al sistema o all'utente stesso. Gli
// utenti autenticati possono creare cartelle nella radice di C:: un compagno di laboratorio che crea C:\php\php-cgi.exe
// farebbe girare il suo programma con l'account di chi avvia SegVault dopo di lui
int sv_win_trusted_path(const char* path) {
    HANDLE tok;
    BYTE buf[256];
    DWORD n = 0;
    PSID me = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) me = ((TOKEN_USER*)buf)->User.Sid;
        CloseHandle(tok);
    }
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (;;) {
        size_t l = strlen(p);
        while (l > 0 && (p[l - 1] == '\\' || p[l - 1] == '/')) p[--l] = '\0';
        if (l <= 2) return 1;                           // "C:": la radice del disco
        if (!owner_trusted(p, me)) return 0;
        char* a = strrchr(p, '\\');
        char* b = strrchr(p, '/');
        char* sl = a > b ? a : b;
        if (!sl) return 1;
        *sl = '\0';
    }
}

// wine non conserva le ACL (le ricava dai permessi Unix): i test lo devono sapere
int sv_win_under_wine(void) {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != NULL;
}
#else
typedef int sv_win_acl_unused;          // su POSIX niente (ISO C vuole almeno una dichiarazione)
#endif
