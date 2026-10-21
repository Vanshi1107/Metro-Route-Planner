# Accounts and security: what is protected, and what is not

## What the code does

- **Passwords** are never stored. Each is turned into `pbkdf2-sha256$600000$<salt>$<hash>`: PBKDF2-HMAC-SHA256,
  600,000 iterations, a random 16-byte salt per person. On Windows this uses the operating system's own
  cryptography (CNG); the tests check it against published reference values.
- **Why PBKDF2 and not Argon2id?** OWASP's Password Storage Cheat Sheet lists Argon2id first and PBKDF2 (600,000+
  iterations, HMAC-SHA-256) as the choice where FIPS-validated, standard-library implementations are wanted. We use
  PBKDF2 because it needs no download and no third-party code. The algorithm and iteration count are stored inside
  every hash, so moving to Argon2id later, or raising the cost, does not lock anyone out: old hashes keep working
  and are upgraded the next time the person logs in.
- **Login answers** are the same ("Wrong username or password.") whether or not the name exists, and take the same
  amount of work, so the answer does not reveal which usernames are real.
- **Lockout:** 5 failed logins within 15 minutes lock that username for 15 minutes. Names that do not exist are
  counted too.
- **Sessions:** a token is 256 random bits from the operating system. Only its SHA-256 is stored, so a leaked
  database cannot be used to take over a session. Sessions last 7 days; each person keeps at most 10.
- **SQL injection:** every piece of text from a person goes through one quoting function, and usernames can only
  be letters and digits. The tests include injection attempts.
- **Journeys** belong to one account, and every operation checks the owner.

## The web layer

- **The server listens on this computer only** (127.0.0.1) unless you change it with `--bind`.
- **Changes need proof they come from our own page:** every POST and DELETE must carry the header
  `X-Requested-With: metro-web`, and if the browser sends an `Origin` it must be this site. A page on another site
  cannot add that header without our permission, so it cannot make your browser change anything here. The session
  cookie is also `SameSite=Strict`.
- **The session cookie** is `HttpOnly` (scripts cannot read it) and `SameSite=Strict`. It is marked `Secure` only
  when the site is served over HTTPS (`ApiConfig::secureCookies`).
- **Content-Security-Policy** on every response: scripts, styles and fonts only from this site, nothing inline,
  no framing, and images only from this site and OpenStreetMap's tile servers. The page contains no inline script,
  so the policy has no `unsafe-inline`.
- **Text is never treated as markup.** The page only uses `textContent`; map tooltips and popups are passed as
  elements, not strings (Leaflet reads plain strings as HTML); the JSON also escapes `<`, `>` and `&`.
- **Limits:** request body 8 KB, address 2 KB, search text 60 characters; malformed percent-encoding is refused.
- **Static files** are served only from `web/`, with a strict name check (no `..`, no hidden files, no doubled or
  trailing slashes, only known file types) and a symbolic-link check.
- **Errors** shown to the browser never contain internal details; they are logged on the server instead.
- **Tests:** `test_web` includes path-traversal and symlink attempts, origin and header checks, and 3,000 random
  requests that must never crash or produce invalid JSON. A real browser run (Chromium) also checked that the
  page raises no Content-Security-Policy violations and that a hostile label cannot run script.

## The login page and saving

- **The forms use `method="post"`.** If the script ever failed to load, pressing Enter must not put the username and
  password in the address bar (where they would be kept in history and logs). A test turns JavaScript off to prove it.
- **Where you are sent after logging in** comes from `?next=`, but only `/` and `/index.html` are accepted. Anything
  else (another site, `//evil.example`, `javascript:`, a path into the API) is replaced by `/`. A test tries each.
- **The submit buttons start switched off** and `auth.js` switches them on. Pressing Enter before the script has loaded does nothing,
  instead of sending the form natively to a page that cannot handle it (which would leave the person on an error page). A test
  holds the script back on purpose and presses Enter. With JavaScript off, the page says it needs JavaScript and the form stays inert.
- **The rules shown while typing are hints.** The server decides what is acceptable and its message is shown as it is.
- **The "log in to save" memory** is kept in the browser's `sessionStorage` (the two stations, the day and the option),
  holds no password and no session, is used once and then deleted, and is checked before use. If storage is switched off,
  the search is simply not restored.
- **Keyboard focus** goes to the first field immediately and is never moved afterwards, so slow loading cannot pull the
  cursor out of a box someone is typing in (this happened once during development, and a test now guards it).
- **A failed login names no field that gave it away:** the message for a wrong username and for a wrong password is the same.

## What it does NOT do (so do not assume it)

- **The map uses OpenStreetMap's public tile servers.** Their policy is for light use, and every tile request tells
  them the viewer's address and (by Referer) which site is being used. A deployed service would need its own tile
  provider.
- **Requests are handled one at a time.** Logging in takes a fraction of a second, during which other requests wait.
  Fine for a demonstration, not for many users at once.
- **No encryption in transit.** Over plain HTTP a password travels readable. This is acceptable only if the server
  is reachable from your own computer alone. A real deployment needs HTTPS.
- **The lockout can be abused:** someone who knows a username can lock that person out by failing on purpose. That
  is the usual trade-off. Counting per network address would help but is not done yet.
- **No password reset, no email check, no two-factor login, no "change password" yet.** The login page does not offer
  "forgot password" because nothing behind it exists.
- **No check against lists of common passwords.** The rule is only: 8 to 128 characters, any characters allowed,
  not equal to the username.
- **No pepper** (a secret kept outside the database). Reasonable to add later.
- **The command-line tool keeps the session token in `data/session.token`.** Anyone who can read that file can act
  as you. The web version will use a cookie instead.
- **Saved-journey labels are free text.** When the web page shows them they must be escaped, or they could inject
  script into the page. That is a requirement for the web milestone.
- **Passwords typed into the command-line tool should be plain ASCII** (the console's code page, not UTF-8, is
  what the tool receives). The web version will accept any language.
