/* MetroGo: the log in / create account page.
 *
 * Safety rules for this file:
 *  - Text from the server is only put on the page with textContent.
 *  - The page you are sent to after logging in comes from ?next=, but only these exact paths are accepted, so a crafted
 *    link can never send someone to another site.
 *  - Every change sends the header X-Requested-With: metro-web (the server requires it).
 *  - The rules shown while typing are only hints. The server decides what is acceptable.
 */
(function () {
  'use strict';

  var NEXT_OK = ['/', '/index.html', '/journeys.html'];

  function $(id) { return document.getElementById(id); }

  function nextPage() {
    var n = new URLSearchParams(window.location.search).get('next');
    return NEXT_OK.indexOf(n) >= 0 ? n : '/';
  }

  function api(method, path, params) {
    var opts = { method: method, credentials: 'same-origin', headers: { 'X-Requested-With': 'metro-web' } };
    var url = path;
    var body = params ? new URLSearchParams(params).toString() : '';
    if (method === 'GET') {
      if (body) url += '?' + body;
    } else if (body) {
      opts.headers['Content-Type'] = 'application/x-www-form-urlencoded';
      opts.body = body;
    }
    return fetch(url, opts).then(function (res) {
      return res.json().catch(function () { return null; }).then(function (data) {
        if (!res.ok) {
          var err = new Error((data && data.error) || 'Something went wrong (' + res.status + ').');
          err.status = res.status;
          throw err;
        }
        return data;
      });
    });
  }

  // ----------------------------------------------------------------------------------------------- messages

  function showMessage(text) {
    var m = $('form-message');
    m.textContent = text;
    m.hidden = !text;
  }

  function markInvalid(id) {
    ['login-user', 'login-pass', 'reg-user', 'reg-pass', 'reg-confirm'].forEach(function (f) {
      if ($(f)) $(f).removeAttribute('aria-invalid');
    });
    if (id && $(id)) {
      $(id).setAttribute('aria-invalid', 'true');
      $(id).focus();
    }
  }

  function fail(message, fieldId) {
    showMessage(message);
    markInvalid(fieldId);
  }

  function busy(button, isBusy, idleText) {
    button.disabled = isBusy;
    button.textContent = isBusy ? 'Please wait\u2026' : idleText;
  }

  // ----------------------------------------------------------------------------------------------- tabs

  function setMode(mode, focusField) {
    var reg = mode === 'register';
    $('login-form').hidden = reg;
    $('register-form').hidden = !reg;
    $('tab-login').setAttribute('aria-selected', reg ? 'false' : 'true');
    $('tab-register').setAttribute('aria-selected', reg ? 'true' : 'false');
    $('tab-login').tabIndex = reg ? -1 : 0;
    $('tab-register').tabIndex = reg ? 0 : -1;
    $('auth-heading').textContent = reg ? 'Create your account' : 'Welcome back';
    $('auth-sub').textContent = reg ? 'It is free. Save journeys and open them again any time.' : 'Log in to see and save your journeys.';
    document.title = (reg ? 'Create account' : 'Log in') + ' \u2013 MetroGo';
    showMessage('');
    markInvalid(null);
    try {
      window.history.replaceState(null, '', window.location.pathname + window.location.search + (reg ? '#register' : ''));
    } catch (e) { /* not important */ }
    if (focusField) $(reg ? 'reg-user' : 'login-user').focus();
  }

  // ----------------------------------------------------------------------------------------------- hints while typing

  function charCount(s) { return Array.from(s).length; }   // characters, not bytes, as the server counts them

  function updateRules() {
    var u = $('reg-user').value, p = $('reg-pass').value, c = $('reg-confirm').value;
    var met = {
      ulen: u.length >= 3 && u.length <= 20,
      uchars: u.length > 0 && /^[A-Za-z0-9]+$/.test(u),
      plen: charCount(p) >= 8 && charCount(p) <= 128,
      pname: p.length > 0 && p.toLowerCase() !== u.toLowerCase(),
      pmatch: p.length > 0 && p === c
    };
    Array.prototype.forEach.call($('register-form').querySelectorAll('.checklist li'), function (li) {
      var ok = !!met[li.getAttribute('data-rule')];
      li.classList.toggle('ok', ok);
      li.setAttribute('data-state', ok ? 'met' : 'not yet');   // shown to screen readers through CSS content
    });
  }

  function wirePasswordToggle(buttonId, fieldIds) {
    var btn = $(buttonId);
    btn.addEventListener('click', function () {
      var show = btn.getAttribute('aria-pressed') !== 'true';
      btn.setAttribute('aria-pressed', show ? 'true' : 'false');
      btn.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
      btn.textContent = show ? 'Hide' : 'Show';
      fieldIds.forEach(function (id) { $(id).type = show ? 'text' : 'password'; });
    });
  }

  // ----------------------------------------------------------------------------------------------- actions

  function goOn() { window.location.assign(nextPage()); }

  function onLogin(e) {
    e.preventDefault();
    var user = $('login-user').value.trim(), pass = $('login-pass').value;
    if (!user || !pass) { fail('Enter your username and password.', user ? 'login-pass' : 'login-user'); return; }
    var btn = $('login-submit');
    busy(btn, true, 'Log in');
    showMessage('');
    api('POST', '/api/login', { username: user, password: pass }).then(goOn).catch(function (err) {
      busy(btn, false, 'Log in');
      fail(err.message, err.status === 401 ? 'login-pass' : null);
      if (err.status === 401) $('login-pass').select();
    });
  }

  function onRegister(e) {
    e.preventDefault();
    var user = $('reg-user').value.trim(), pass = $('reg-pass').value, again = $('reg-confirm').value;
    if (pass !== again) { fail('The two passwords are not the same.', 'reg-confirm'); return; }
    var btn = $('register-submit');
    busy(btn, true, 'Create account');
    showMessage('');
    api('POST', '/api/register', { username: user, password: pass }).then(function () {
      return api('POST', '/api/login', { username: user, password: pass });
    }).then(goOn).catch(function (err) {
      busy(btn, false, 'Create account');
      var field = err.status === 409 ? 'reg-user' : (/username/i.test(err.message) ? 'reg-user' : 'reg-pass');
      fail(err.message, err.status === 429 ? null : field);
    });
  }

  function showSignedIn(name) {
    $('signed-name').textContent = name;
    $('go-planner').setAttribute('href', nextPage());
    $('auth-forms').hidden = true;
    $('signed-in').hidden = false;
    $('auth-heading').textContent = 'You are signed in';
    $('auth-sub').textContent = '';
    document.title = 'Signed in \u2013 MetroGo';
  }

  function start() {
    $('tab-login').addEventListener('click', function () { setMode('login', true); });
    $('tab-register').addEventListener('click', function () { setMode('register', true); });
    $('to-register').addEventListener('click', function () { setMode('register', true); });
    $('to-login').addEventListener('click', function () { setMode('login', true); });
    $('tab-login').parentNode.addEventListener('keydown', function (e) {   // arrow keys move between the two tabs
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
      var toRegister = $('tab-login').getAttribute('aria-selected') === 'true';
      setMode(toRegister ? 'register' : 'login', false);
      $(toRegister ? 'tab-register' : 'tab-login').focus();
      e.preventDefault();
    });
    $('login-form').addEventListener('submit', onLogin);
    $('register-form').addEventListener('submit', onRegister);
    ['reg-user', 'reg-pass', 'reg-confirm'].forEach(function (id) { $(id).addEventListener('input', updateRules); });
    ['login-user', 'login-pass', 'reg-user', 'reg-pass', 'reg-confirm'].forEach(function (id) {
      $(id).addEventListener('input', function () { $(id).removeAttribute('aria-invalid'); });
    });
    // The address can change without the page reloading (typing login.html#register while already here, or Back and
    // Forward). Follow it, so the tab shown always matches the address.
    window.addEventListener('hashchange', function () {
      var wanted = window.location.hash === '#register' ? 'register' : 'login';
      var shown = $('tab-register').getAttribute('aria-selected') === 'true' ? 'register' : 'login';
      if (wanted !== shown && $('auth-forms').hidden === false) setMode(wanted, true);
    });
    wirePasswordToggle('login-toggle', ['login-pass']);
    wirePasswordToggle('reg-toggle', ['reg-pass', 'reg-confirm']);
    $('signed-logout').addEventListener('click', function () {
      api('POST', '/api/logout').catch(function () {}).then(function () { window.location.reload(); });
    });

    $('login-submit').disabled = false;       // the buttons start switched off in the HTML; they work only once this script is running
    $('register-submit').disabled = false;
    updateRules();
    setMode(window.location.hash === '#register' ? 'register' : 'login', false);
    // Focus the first field NOW. Doing it after the "am I logged in?" answer arrives would pull the cursor away from
    // whatever the person has already started typing in.
    $(window.location.hash === '#register' ? 'reg-user' : 'login-user').focus();
    api('GET', '/api/me').then(function (me) {
      if (me.loggedIn) showSignedIn(me.username);
    }).catch(function () { /* the forms still work */ });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
