// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/atom_browser_client.h"

#include "atom/browser/atom_access_token_store.h"
#include "atom/browser/atom_browser_context.h"
#include "atom/browser/atom_browser_main_parts.h"
#include "atom/browser/atom_quota_permission_context.h"
#include "atom/browser/atom_speech_recognition_manager_delegate.h"
#include "atom/browser/native_window.h"
#include "atom/browser/web_view_manager.h"
#include "atom/browser/window_list.h"
#include "atom/common/options_switches.h"
#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/printing/printing_message_filter.h"
#include "chrome/browser/renderer_host/pepper/chrome_browser_pepper_host_factory.h"
#include "chrome/browser/speech/tts_message_filter.h"
#include "content/public/browser/browser_ppapi_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/web_preferences.h"
#include "ppapi/host/ppapi_host.h"
#include "ui/base/l10n/l10n_util.h"

namespace atom {

namespace {

// The default routing id of WebContents.
// In Electron each RenderProcessHost only has one WebContents, so this ID is
// same for every WebContents.
int kDefaultRoutingID = 2;

// Next navigation should not restart renderer process.
bool g_suppress_renderer_process_restart = false;

// Find out the owner of the child process according to |process_id|.
enum ProcessOwner {
  OWNER_NATIVE_WINDOW,
  OWNER_GUEST_WEB_CONTENTS,
  OWNER_NONE,  // it might be devtools though.
};
ProcessOwner GetProcessOwner(int process_id,
                             NativeWindow** window,
                             WebViewManager::WebViewInfo* info) {
  auto web_contents = content::WebContents::FromRenderViewHost(
      content::RenderViewHost::FromID(process_id, kDefaultRoutingID));
  if (!web_contents)
    return OWNER_NONE;

  // First search for NativeWindow.
  for (auto native_window : *WindowList::GetInstance())
    if (web_contents == native_window->GetWebContents()) {
      *window = native_window;
      return OWNER_NATIVE_WINDOW;
    }

  // Then search for guest WebContents.
  if (WebViewManager::GetInfoForWebContents(web_contents, info))
    return OWNER_GUEST_WEB_CONTENTS;

  return OWNER_NONE;
}

}  // namespace

// static
void AtomBrowserClient::SuppressRendererProcessRestartForOnce() {
  g_suppress_renderer_process_restart = true;
}

AtomBrowserClient::AtomBrowserClient() {
}

AtomBrowserClient::~AtomBrowserClient() {
}

void AtomBrowserClient::RenderProcessWillLaunch(
    content::RenderProcessHost* host) {
  int id = host->GetID();
  host->AddFilter(new printing::PrintingMessageFilter(host->GetID()));
  host->AddFilter(new TtsMessageFilter(id, host->GetBrowserContext()));
}

content::SpeechRecognitionManagerDelegate*
    AtomBrowserClient::CreateSpeechRecognitionManagerDelegate() {
  return new AtomSpeechRecognitionManagerDelegate;
}

content::AccessTokenStore* AtomBrowserClient::CreateAccessTokenStore() {
  return new AtomAccessTokenStore;
}

void AtomBrowserClient::OverrideWebkitPrefs(
    content::RenderViewHost* host, content::WebPreferences* prefs) {
  prefs->javascript_enabled = true;
  prefs->web_security_enabled = true;
  prefs->javascript_can_open_windows_automatically = true;
  prefs->plugins_enabled = true;
  prefs->dom_paste_enabled = true;
  prefs->java_enabled = false;
  prefs->allow_scripts_to_close_windows = true;
  prefs->javascript_can_access_clipboard = true;
  prefs->local_storage_enabled = true;
  prefs->databases_enabled = true;
  prefs->application_cache_enabled = true;
  prefs->allow_universal_access_from_file_urls = true;
  prefs->allow_file_access_from_file_urls = true;
  prefs->experimental_webgl_enabled = true;
  prefs->allow_displaying_insecure_content = false;
  prefs->allow_running_insecure_content = false;

  // Custom preferences of guest page.
  auto web_contents = content::WebContents::FromRenderViewHost(host);
  WebViewManager::WebViewInfo info;
  if (WebViewManager::GetInfoForWebContents(web_contents, &info)) {
    prefs->web_security_enabled = !info.disable_web_security;
    return;
  }

  NativeWindow* window = NativeWindow::FromWebContents(web_contents);
  if (window)
    window->OverrideWebkitPrefs(prefs);
}

std::string AtomBrowserClient::GetApplicationLocale() {
  return l10n_util::GetApplicationLocale("");
}

void AtomBrowserClient::OverrideSiteInstanceForNavigation(
    content::BrowserContext* browser_context,
    content::SiteInstance* current_instance,
    const GURL& url,
    content::SiteInstance** new_instance) {
  if (g_suppress_renderer_process_restart) {
    g_suppress_renderer_process_restart = false;
    return;
  }

  // Restart renderer process for all navigations except "javacript:" scheme.
  if (url.SchemeIs(url::kJavaScriptScheme))
    return;

  *new_instance = content::SiteInstance::CreateForURL(browser_context, url);
}

void AtomBrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int process_id) {
  std::string process_type = command_line->GetSwitchValueASCII("type");
  if (process_type != "renderer")
    return;

  NativeWindow* window;
  WebViewManager::WebViewInfo info;
  ProcessOwner owner = GetProcessOwner(process_id, &window, &info);

  if (owner == OWNER_NATIVE_WINDOW) {
    window->AppendExtraCommandLineSwitches(command_line);
  } else if (owner == OWNER_GUEST_WEB_CONTENTS) {
    command_line->AppendSwitchASCII(
        switches::kGuestInstanceID, base::IntToString(info.guest_instance_id));
    command_line->AppendSwitchASCII(
        switches::kNodeIntegration, info.node_integration ? "true" : "false");
    if (info.plugins)
      command_line->AppendSwitch(switches::kEnablePlugins);
    if (!info.preload_script.empty())
      command_line->AppendSwitchPath(
          switches::kPreloadScript, info.preload_script);
  }
}

void AtomBrowserClient::DidCreatePpapiPlugin(
    content::BrowserPpapiHost* browser_host) {
  auto command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kEnablePlugins))
    browser_host->GetPpapiHost()->AddHostFactoryFilter(
        scoped_ptr<ppapi::host::HostFactory>(
            new chrome::ChromeBrowserPepperHostFactory(browser_host)));
}

content::QuotaPermissionContext*
    AtomBrowserClient::CreateQuotaPermissionContext() {
  return new AtomQuotaPermissionContext;
}

brightray::BrowserMainParts* AtomBrowserClient::OverrideCreateBrowserMainParts(
    const content::MainFunctionParams&) {
  v8::V8::Initialize();  // Init V8 before creating main parts.
  return new AtomBrowserMainParts;
}

}  // namespace atom
