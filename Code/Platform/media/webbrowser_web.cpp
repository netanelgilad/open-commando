/*
** WebBrowser without an embedded browser.
**
** On Windows this hosts Internet Explorer (via WOLBrowser.dll) to show
** Westwood Online web pages. The browser build can't embed IE, so no
** instance is ever created: CreateInstance() returns NULL, which callers
** already treat as "browser unavailable". The remaining members exist so
** the class links; they report failure.
*/
#include "WebBrowser.h"

WebBrowser *WebBrowser::_mInstance = NULL;

bool WebBrowser::IsWebPageDisplayed(void) { return false; }

WebBrowser *WebBrowser::CreateInstance(HWND) { return NULL; }

WebBrowser::WebBrowser() : mRefCount(1), mVisible(false), mSwitchedMode(false), mRestoreWidth(0), mRestoreHeight(0), mRestoreBits(0) {
  mPendingURL[0] = 0;
  memset(&mProcessInfo, 0, sizeof(mProcessInfo));
}

WebBrowser::~WebBrowser() {}

bool WebBrowser::IsExternalBrowserRunning(void) const { return false; }
bool WebBrowser::ShowWebPage(const char *) { return false; }
bool WebBrowser::LaunchExternal(const char *) { return false; }
void WebBrowser::Show(void) { mVisible = false; }
void WebBrowser::Hide(void) { mVisible = false; }
bool WebBrowser::FinalizeCreate(HWND) { return false; }
bool WebBrowser::RetrievePageURL(const char *, char *url, int size) {
  if (url && size > 0)
    url[0] = 0;
  return false;
}
bool WebBrowser::RetrieveHTMLPath(char *path, int size) {
  if (path && size > 0)
    path[0] = 0;
  return false;
}

HRESULT STDMETHODCALLTYPE WebBrowser::QueryInterface(REFIID, void **ppvObject) {
  if (ppvObject)
    *ppvObject = NULL;
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE WebBrowser::AddRef(void) { return ++mRefCount; }
ULONG STDMETHODCALLTYPE WebBrowser::Release(void) {
  ULONG count = --mRefCount;
  if (count == 0)
    delete this;
  return count;
}

STDMETHODIMP WebBrowser::OnScriptQuit(void) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnBeforeNavigate(const wchar_t *, const wchar_t *) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnDocumentComplete(const wchar_t *, BOOL) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnDownloadBegin(void) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnProgressChange(LONG, LONG) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnDownloadComplete(void) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnNavigateComplete(const wchar_t *) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnStatusTextChange(const wchar_t *) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnTitleChange(const wchar_t *) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnNewWindow(void) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnShowMessage(const wchar_t *, const wchar_t *, ULONG, LONG *) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnFailedPageCertification(void) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnErrorMsg(const wchar_t *) { return E_NOTIMPL; }
STDMETHODIMP WebBrowser::OnRegisterLogin(const wchar_t *, const wchar_t *) { return E_NOTIMPL; }
