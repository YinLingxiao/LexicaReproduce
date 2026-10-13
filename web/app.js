const ICONS = {
	search: "m21 21-4.5-4.5 M19 10.5a8.5 8.5 0 1 1-17 0 8.5 8.5 0 0 1 17 0",
	bookmark: "M6 3h12v18l-6-4-6 4V3Z",
	arrow: "M4 12h16 m-6-6 6 6-6 6",
	check: "m5 12 4 4L19 6",
	close: "m6 6 12 12 M6 18 18 6",
	sound: "M11 4 5 9H2v6h3l6 5V4Zm4 4c2 2 2 6 0 8 M18 5c4 4 4 10 0 14",
	copy: "M8 8h12v13H8V8Z M16 8V3H3v13h5",
	back: "M20 12H4m6-6-6 6 6 6",
	spark: "m12 3 2.5 6.5L21 12l-6.5 2.5L12 21l-2.5-6.5L3 12l6.5-2.5L12 3Z",
	book: "M12 6c-3-3-7-3-10-2v15c4-1 7 0 10 2 3-2 6-3 10-2V4c-3-1-7-1-10 2Zm0 0v15",
	sun: "M12 2v2 M12 20v2 M2 12h2 M20 12h2 M5 5l1 1 M18 18l1 1 M5 19l1-1 M18 6l1-1 M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0",
	moon: "M20 15A9 9 0 0 1 9 4a9 9 0 1 0 11 11Z"
};
const STAGES = ["New", "Learning", "Familiar", "Stable"];
const FILTERS = [
	["all", "All", ""],
	["saved", "Saved", "fav=1"],
	["0", "New", "stage=0"],
	["1", "Learning", "stage=1"],
	["2", "Familiar", "stage=2"],
	["3", "Stable", "stage=3"],
	["paused", "Paused", "paused=1"]
];
const TITLES = { "/": "Search", "/library": "Words", "/review": "Review", "/stats": "Stats", "/settings": "Settings" };
const prefs = { theme: "system", reading: "guided", fontSize: "normal" };
const suggest = { rows: [], open: false, index: -1, gen: 0, timer: 0, error: "" };
const lib = { q: "", filter: "all", page: 1, timer: 0, gen: 0 };
const word = {};
const review = { phase: "loading", session: "", question: null, hints: [], grade: null, shown: "", summary: null, answered: 0, error: "", busy: false };
let viewGen = 0;
let reviewToken = 0;
let focusSearch = false;

function icon(name, size) {
	return `<svg width="${size || 16}" height="${size || 16}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="${ICONS[name] || ICONS.book}"/></svg>`;
}
function esc(value) {
	return String(value ?? "").replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch]));
}
function route() {
	const raw = location.hash.length > 1 ? location.hash.slice(1) : "/";
	const url = new URL(raw, location.origin);
	return { path: url.pathname, params: url.searchParams };
}
function go(path) {
	if (!path.startsWith("/")) path = "/" + path;
	const current = location.hash.startsWith("#") ? decodeURIComponent(location.hash.slice(1)) : "";
	if (current === path) render();
	else location.hash = path;
}
function errText(error) {
	const code = error && error.code ? ` (${error.code})` : "";
	return ((error && error.message) || "Request failed") + code;
}
async function api(method, path, body) {
	const res = await fetch(path, {
		method,
		headers: body === undefined ? {} : { "Content-Type": "application/json" },
		body: body === undefined ? undefined : JSON.stringify(body)
	});
	let json = null;
	try { json = await res.json(); } catch { json = null; }
	if (!json || json.ok !== true) {
		const error = new Error((json && json.error && json.error.message) || "Request failed");
		error.code = (json && json.error && json.error.code) || "BAD_REQUEST";
		throw error;
	}
	return json.data;
}
function loadPrefs() {
	try {
		const value = JSON.parse(localStorage.getItem("lexica.preferences") || "{}");
		if (["light", "dark", "system"].includes(value.theme)) prefs.theme = value.theme;
		if (["guided", "full"].includes(value.reading)) prefs.reading = value.reading;
		if (["normal", "large"].includes(value.fontSize)) prefs.fontSize = value.fontSize;
	} catch { /* keep defaults */ }
}
function applyPrefs() {
	document.documentElement.dataset.theme = prefs.theme;
	document.documentElement.dataset.fontSize = prefs.fontSize;
	try { localStorage.setItem("lexica.preferences", JSON.stringify(prefs)); } catch { /* session only */ }
	const dark = prefs.theme === "dark" || (prefs.theme === "system" && matchMedia("(prefers-color-scheme: dark)").matches);
	const theme = document.getElementById("theme");
	if (theme) theme.innerHTML = icon(dark ? "sun" : "moon", 16);
}
function relTime(utc) {
	const min = Math.floor((Date.now() / 1000 - utc) / 60);
	if (!Number.isFinite(min)) return "";
	if (min < 1) return "just now";
	if (min < 60) return min + "m ago";
	const h = Math.floor(min / 60);
	if (h < 24) return h + "h ago";
	return Math.floor(h / 24) + "d ago";
}
function opFor(text) {
	const key = text.trim().toLowerCase();
	let saved = null;
	try { saved = JSON.parse(sessionStorage.getItem("lexica.lookup.op") || "null"); } catch { saved = null; }
	if (saved && saved.word === key && saved.opId) return saved.opId;
	const opId = crypto.randomUUID();
	try { sessionStorage.setItem("lexica.lookup.op", JSON.stringify({ word: key, opId })); } catch { /* still send it */ }
	return opId;
}
function content() { return document.getElementById("content"); }
function updateChrome(path) {
	document.title = (TITLES[path] || "Search") + " · Lexica";
	document.querySelectorAll("[data-nav]").forEach((link) => {
		const on = link.dataset.nav === path;
		link.classList.toggle("active", on);
		if (on) link.setAttribute("aria-current", "page");
		else link.removeAttribute("aria-current");
	});
}
function searchBox(value) {
	return `<div class="search"><span class="search-icon">${icon("search", 22)}</span><input data-search value="${esc(value)}" role="combobox" aria-label="Search for a word" aria-autocomplete="list" aria-expanded="false" aria-controls="suggest" placeholder="Search a word…" autocomplete="off" spellcheck="false"><div class="search-actions">${value ? `<button class="icon-button ghost" type="button" data-act="clear-search" aria-label="Clear search">${icon("close", 16)}</button>` : "<kbd>Ctrl K</kbd>"}<button class="primary submit" type="button" data-act="commit-search" aria-label="Look up">${icon("arrow", 20)}</button></div><ul class="suggestions" id="suggest" role="listbox" aria-label="Suggestions" hidden></ul></div><p id="search-error" class="small muted" role="status"></p>`;
}
function paintSuggest() {
	const list = document.getElementById("suggest");
	const input = document.querySelector("[data-search]");
	const note = document.getElementById("search-error");
	if (!list || !input) return;
	input.setAttribute("aria-expanded", suggest.open ? "true" : "false");
	input.setAttribute("aria-activedescendant", suggest.open && suggest.index >= 0 ? "suggest-" + suggest.index : "");
	if (!suggest.open) { list.hidden = true; list.innerHTML = ""; }
	else {
		list.hidden = false;
		list.innerHTML = suggest.rows.map((row, i) => `<li id="suggest-${i}" role="option" aria-selected="${i === suggest.index}" class="${i === suggest.index ? "selected" : ""}" data-act="pick" data-word="${esc(row.word)}"><span><strong>${esc(row.word)}</strong></span>${icon("arrow", 16)}</li>`).join("") + `<li class="search-help" role="presentation">↑ ↓ select · Enter look up · Esc close</li>`;
	}
	if (note) note.textContent = suggest.error;
}
function commitSearch(text) {
	const q = (text || "").trim();
	if (!q) return;
	suggest.gen += 1;
	suggest.open = false;
	clearTimeout(suggest.timer);
	go("/?word=" + encodeURIComponent(q));
}
function onSearchInput(input) {
	clearTimeout(suggest.timer);
	const token = ++suggest.gen;
	const query = input.value.trim();
	suggest.open = false;
	suggest.index = -1;
	suggest.error = "";
	const clear = input.parentElement.querySelector("[data-act='clear-search']");
	const kbd = input.parentElement.querySelector("kbd");
	if (query && !clear) {
		if (kbd) kbd.outerHTML = `<button class="icon-button ghost" type="button" data-act="clear-search" aria-label="Clear search">${icon("close", 16)}</button>`;
	} else if (!query && clear) clear.outerHTML = "<kbd>Ctrl K</kbd>";
	paintSuggest();
	if (!query) { suggest.rows = []; return; }
	suggest.timer = setTimeout(async () => {
		try {
			const rows = await api("GET", "/api/suggest?q=" + encodeURIComponent(query));
			if (token !== suggest.gen) return;
			suggest.rows = rows;
			suggest.open = rows.length > 0;
		} catch {
			if (token !== suggest.gen) return;
			suggest.rows = [];
			suggest.error = "Suggestions unavailable — press Enter to look up.";
		}
		paintSuggest();
	}, 120);
}
function onSearchKey(event) {
	if (event.isComposing) return;
	if (event.key === "ArrowDown" && suggest.open) {
		event.preventDefault();
		suggest.index = Math.min(suggest.index + 1, suggest.rows.length - 1);
		paintSuggest();
	} else if (event.key === "ArrowUp" && suggest.open) {
		event.preventDefault();
		suggest.index = Math.max(suggest.index - 1, -1);
		paintSuggest();
	} else if (event.key === "Enter") {
		event.preventDefault();
		const picked = suggest.open && suggest.index >= 0 ? suggest.rows[suggest.index].word : event.target.value;
		commitSearch(picked);
	} else if (event.key === "Escape") {
		event.preventDefault();
		if (suggest.open) { suggest.gen += 1; suggest.open = false; paintSuggest(); }
		else { event.target.value = ""; onSearchInput(event.target); }
	}
}
function num(value) { return Number(value || 0).toLocaleString(); }
async function pageSearch(gen, query) {
	const root = content();
	suggest.open = false;
	if (!query) {
		root.innerHTML = `<main class="page home"><div class="hero">${searchBox("")}<div class="hero-foot"><span id="dict-line">—</span><span><kbd>/</kbd> to search</span></div></div><div id="home-rest"></div></main>`;
		if (focusSearch) { focusSearch = false; document.querySelector("[data-search]")?.focus(); }
		const [statusR, statsR, histR] = await Promise.allSettled([
			api("GET", "/api/status"),
			api("GET", "/api/stats"),
			api("GET", "/api/history?page=1&size=8")
		]);
		if (gen !== viewGen) return;
		const status = statusR.status === "fulfilled" ? statusR.value : null;
		const stats = statsR.status === "fulfilled" ? statsR.value : null;
		const history = histR.status === "fulfilled" ? histR.value : { total: 0, items: [] };
		const line = document.getElementById("dict-line");
		if (line) line.textContent = !status ? "Offline" : status.dict_ready ? status.state + " · dictionary ready" : status.state;
		const rest = document.getElementById("home-rest");
		if (!rest) return;
		let html = "";
		if (status && !status.dict_ready) {
			html += `<section class="panel empty"><div class="empty-icon">${icon("book", 26)}</div><h2>Dictionary not imported</h2><p>Lookups stay unavailable until an ECDICT file is imported.</p><a class="button primary" href="#/settings">Import dictionary ${icon("arrow", 16)}</a></section>`;
		}
		if (stats) {
			const words = stats.stage0 + stats.stage1 + stats.stage2 + stats.stage3;
			html += `<div class="stat-row"><div><span class="stat-label">Words</span><span class="stat-value">${num(words)}</span></div><div><span class="stat-label">Familiar</span><span class="stat-value">${num(stats.stage2 + stats.stage3)}</span></div><div><span class="stat-label">Stable</span><span class="stat-value">${num(stats.stage3)}</span></div><a class="due" href="#/review"><span class="stat-label">Review</span><span class="stat-value">Open ${icon("arrow", 15)}</span></a></div>`;
		}
		if (history.items.length) {
			html += `<section class="recent"><div class="section-heading"><h2>Recent</h2><a href="#/library">All words</a></div><div class="recent-grid">${history.items.map((item) => `<a href="#/?word=${encodeURIComponent(item.word)}"><span><strong>${esc(item.word)}</strong></span><span class="when">${esc(relTime(item.utc))}</span></a>`).join("")}</div></section>`;
		} else if (!status || status.dict_ready) {
			html += `<section class="recent"><div class="section-heading"><h2>Start here</h2></div><div class="recent-grid">${["meticulous", "subtle", "resilient", "profound"].map((item) => `<button type="button" data-act="lookup" data-word="${item}"><span><strong>${item}</strong></span></button>`).join("")}</div></section>`;
		}
		rest.innerHTML = html;
		return;
	}
	root.innerHTML = `<main class="page home"><a class="back" href="#/">${icon("back", 15)}Search</a>${searchBox(query)}<div class="word-area" id="word-area" aria-busy="true"><div class="skeleton"></div></div></main>`;
	const area = document.getElementById("word-area");
	try {
		const entry = await api("POST", "/api/lookup", { word: query, op_id: opFor(query) });
		if (gen !== viewGen) return;
		await showEntry(entry, query, gen);
	} catch (error) {
		if (gen !== viewGen || !area) return;
		if (error.code === "WORD_NOT_FOUND") {
			let rows = [];
			try { rows = await api("GET", "/api/suggest?q=" + encodeURIComponent(query)); } catch { rows = []; }
			if (gen !== viewGen) return;
			area.innerHTML = `<div class="empty"><div class="empty-icon">${icon("search", 26)}</div><h2>Not found</h2><p>No entry for “${esc(query)}”.</p>${rows.length ? `<div class="related">${rows.map((row) => `<button type="button" data-act="lookup" data-word="${esc(row.word)}">${esc(row.word)}</button>`).join("")}</div>` : ""}</div>`;
			return;
		}
		if (error.code === "DICT_NOT_READY") {
			area.innerHTML = `<div class="empty"><div class="empty-icon">${icon("book", 26)}</div><h2>Dictionary not imported</h2><p>${esc(errText(error))}</p><a class="button primary" href="#/settings">Import dictionary</a></div>`;
			return;
		}
		area.innerHTML = `<div class="error" role="alert">${esc(errText(error))}</div><button type="button" data-act="lookup" data-word="${esc(query)}">Try again</button>`;
	}
}
async function findVocab(entry) {
	for (let page = 1; page <= 5; page += 1) {
		const data = await api("GET", `/api/vocab?q=${encodeURIComponent(entry.word)}&page=${page}&size=100`);
		const row = data.items.find((item) => item.id === entry.id);
		if (row) return row;
		if (data.items.length < 100) break;
	}
	return { id: entry.id, word: entry.word, favorite: false, note: "", paused: false, stage: 0 };
}
function entryHtml() {
	const entry = word.entry;
	const row = word.vocab || { favorite: false, note: "", paused: false, stage: 0 };
	const stage = STAGES[row.stage] || "New";
	const guided = prefs.reading === "guided" && !word.showFull;
	const lemma = word.query && word.query.toLowerCase() !== entry.word.toLowerCase();
	const english = word.showEnglish || word.showFull || prefs.reading === "full";
	return `<article class="word-layout"><div class="main-column"><section class="panel word-card"><div class="word-top"><div><h1>${esc(entry.word)}</h1>${entry.phonetic ? `<p class="phonetic">${esc(entry.phonetic)} <button class="icon-button ghost" type="button" data-act="speak" aria-label="Pronounce">${icon("sound", 16)}</button></p>` : ""}</div><div class="word-tools"><button class="icon-button ${row.favorite ? "saved" : ""}" type="button" data-act="fav" aria-pressed="${row.favorite}" aria-label="Bookmark">${icon("bookmark", 16)}</button><button class="icon-button" type="button" data-act="copy" aria-label="Copy entry">${icon("copy", 16)}</button></div></div><div class="word-meta"><span class="badge ${row.paused ? "paused" : ""}">${row.paused ? "Paused" : esc(stage)}</span>${lemma ? `<span class="badge">from ${esc(word.query)}</span>` : ""}${entry.query && entry.query !== entry.word ? `<span class="badge">lemma</span>` : ""}</div><div class="context" id="ai-box">${word.aiText ? `<p class="example">${esc(word.aiText)}</p><span class="ai-source">AI example</span>` : `<button class="ghost" type="button" data-act="ai" ${word.aiBusy ? "disabled" : ""}>${icon("spark", 14)} ${word.aiBusy ? "Generating…" : "Generate example"}</button>${word.aiError ? `<p class="small muted">${esc(word.aiError)}</p>` : ""}`}</div>${guided && !english ? `<div class="disclosure-actions"><button type="button" data-act="english">Show English</button><button type="button" data-act="full">Full entry</button></div>` : `<div class="definitions"><h2>Definition</h2>${english && entry.definition ? `<p class="english reading">${esc(entry.definition)}</p>` : ""}${(!guided || word.showFull) && entry.translation ? `<p class="chinese reading">${esc(entry.translation)}</p>` : ""}${!entry.definition && !entry.translation ? `<p class="muted">No definition stored.</p>` : ""}${guided && english && !word.showFull ? `<div class="disclosure-actions"><button type="button" data-act="full">Show translation</button></div>` : ""}</div>`}</section>${word.message ? `<p class="save-message" role="status">${icon("check", 16)}${esc(word.message)}</p>` : ""}${word.error ? `<p class="error" role="alert">${esc(word.error)}</p>` : ""}</div><aside><section class="panel notes"><h2>Note</h2><textarea id="note" maxlength="5000">${esc(row.note || "")}</textarea><div class="note-footer"><small id="note-count">${(row.note || "").length} / 5000</small><button type="button" data-act="save-note">Save</button></div><button class="track" type="button" data-act="pause">${row.paused ? "Resume review" : "Pause review"}</button></section></aside></article>`;
}
async function showEntry(entry, query, gen) {
	word.entry = entry;
	word.query = query;
	word.showEnglish = prefs.reading === "full";
	word.showFull = prefs.reading === "full";
	word.aiText = "";
	word.aiError = "";
	word.aiBusy = false;
	word.message = "";
	word.error = "";
	word.vocab = { favorite: false, note: "", paused: false, stage: 0 };
	const area = document.getElementById("word-area");
	if (area) { area.removeAttribute("aria-busy"); area.innerHTML = entryHtml(); }
	try {
		const row = await findVocab(entry);
		if (gen !== viewGen) return;
		word.vocab = row;
		if (area) area.innerHTML = entryHtml();
	} catch (error) {
		if (gen !== viewGen) return;
		word.error = errText(error);
		if (area) area.innerHTML = entryHtml();
	}
}
function paintWord() {
	const area = document.getElementById("word-area");
	if (area && word.entry) area.innerHTML = entryHtml();
}
async function saveVocab(kind, body) {
	if (!word.entry) return;
	word.busy = true;
	word.error = "";
	word.message = "";
	try {
		await api("PUT", `/api/vocab/${word.entry.id}/${kind}`, body);
		word.vocab = await findVocab(word.entry);
		word.message = kind === "favorite" ? (word.vocab.favorite ? "Bookmarked" : "Bookmark removed") : kind === "paused" ? (word.vocab.paused ? "Review paused" : "Review resumed") : "Saved";
	} catch (error) {
		word.error = errText(error);
	} finally {
		word.busy = false;
		paintWord();
	}
}
async function generateExample() {
	if (!word.entry || word.aiBusy) return;
	word.aiBusy = true;
	word.aiError = "";
	paintWord();
	const gen = viewGen;
	try {
		let task = await api("POST", "/api/ai/example", { id: word.entry.id, word: word.entry.word });
		while (task.status === "pending" || task.status === "running") {
			await new Promise((resolve) => setTimeout(resolve, 600));
			if (gen !== viewGen) return;
			task = await api("GET", "/api/ai/task/" + encodeURIComponent(task.id));
		}
		if (gen !== viewGen) return;
		if (task.status !== "succeeded" || !task.text) word.aiError = "Example unavailable.";
		else word.aiText = task.text;
	} catch (error) {
		if (gen !== viewGen) return;
		word.aiError = errText(error);
	} finally {
		word.aiBusy = false;
		if (gen === viewGen) paintWord();
	}
}
function filterQuery() {
	const extra = (FILTERS.find((item) => item[0] === lib.filter) || FILTERS[0])[2];
	return `/api/vocab?page=${lib.page}&size=20&q=${encodeURIComponent(lib.q)}${extra ? "&" + extra : ""}`;
}
async function reloadLibrary() {
	const body = document.getElementById("lib-body");
	const count = document.getElementById("lib-count");
	if (!body) return;
	const token = ++lib.gen;
	body.innerHTML = `<div class="skeleton" role="status" aria-label="Loading words"></div>`;
	try {
		const data = await api("GET", filterQuery());
		if (token !== lib.gen) return;
		const detailed = await Promise.all(data.items.map(async (item) => {
			try { return { item, entry: await api("GET", "/api/entry/" + item.id) }; }
			catch { return { item, entry: null }; }
		}));
		if (token !== lib.gen) return;
		if (count) count.textContent = String(data.total);
		if (!data.items.length) {
			body.innerHTML = `<div class="empty"><div class="empty-icon">${icon(lib.filter === "saved" ? "bookmark" : "book", 26)}</div><h2>${lib.q ? "No matches" : lib.filter === "all" ? "No words yet" : "Nothing here"}</h2><p>${lib.q ? "Try another spelling." : "Every word you look up is collected automatically."}</p>${lib.q || lib.filter !== "all" ? `<button type="button" data-act="lib-clear">Clear filters</button>` : `<a class="button primary" href="#/">Look up a word ${icon("arrow", 16)}</a>`}</div>`;
			return;
		}
		const pages = Math.max(1, Math.ceil(data.total / 20));
		body.innerHTML = `<div class="word-list">${detailed.map(({ item, entry }) => `<div class="word-row"><a class="word-link" href="#/?word=${encodeURIComponent(item.word)}"><div class="word-title"><strong>${esc(item.word)}</strong>${item.favorite ? icon("bookmark", 13) : ""}<span>${esc(entry && entry.phonetic || "")}</span></div><p>${esc((entry && (entry.translation || entry.definition)) || "")}</p>${item.note ? `<small class="note">${esc(item.note)}</small>` : ""}</a><div class="word-status"><span class="badge ${item.paused ? "paused" : ""}">${item.paused ? "Paused" : esc(STAGES[item.stage] || "New")}</span>${item.paused ? `<button class="restore" type="button" data-act="resume" data-id="${item.id}">Resume</button>` : ""}</div></div>`).join("")}</div>${data.total > 20 ? `<div class="pagination"><span>${lib.page} / ${pages}</span><div><button type="button" data-act="lib-prev" ${lib.page <= 1 ? "disabled" : ""}>Previous</button><button type="button" data-act="lib-next" ${lib.page >= pages ? "disabled" : ""}>Next</button></div></div>` : ""}`;
	} catch (error) {
		if (token !== lib.gen) return;
		body.innerHTML = `<div class="error" role="alert">${esc(errText(error))} <button type="button" data-act="lib-retry">Retry</button></div>`;
	}
}
function pageLibrary() {
	content().innerHTML = `<main class="page"><div class="library-top"><h1>Words <span class="count" id="lib-count"></span></h1><div class="filter-search">${icon("search", 16)}<input id="lib-q" aria-label="Search words" placeholder="Filter words…" value="${esc(lib.q)}"></div></div><div class="filters">${FILTERS.map((item) => `<button type="button" class="${lib.filter === item[0] ? "chosen" : ""}" aria-pressed="${lib.filter === item[0]}" data-act="lib-filter" data-filter="${item[0]}">${item[1]}</button>`).join("")}</div><div id="lib-body"><div class="skeleton"></div></div></main>`;
	reloadLibrary();
}
function resetReview() {
	review.phase = "loading";
	review.session = "";
	review.question = null;
	review.hints = [];
	review.grade = null;
	review.shown = "";
	review.summary = null;
	review.answered = 0;
	review.error = "";
	review.busy = false;
}
function gradeCopy(grade) {
	if (grade.duplicate) return "Already submitted";
	if (grade.practice) return grade.correct ? "Correct on practice" : "Practice missed — stage unchanged";
	if (grade.hint_level >= 3) return `Answer revealed · stage ${grade.stage}`;
	if (grade.correct && grade.hint_level > 0) return `Correct with hint ${grade.hint_level} · stage ${grade.stage}`;
	if (grade.correct) return `Correct · stage ${grade.stage}`;
	return `Not mastered · stage ${grade.stage}`;
}
function reviewAlive(token) {
	return token === reviewToken && route().path === "/review";
}
function paintReview() {
	if (route().path !== "/review") return;
	const root = content();
	if (review.phase === "loading") {
		root.innerHTML = `<main class="page review-page"><div class="skeleton" role="status" aria-label="Loading review"></div></main>`;
		return;
	}
	if (review.phase === "error") {
		root.innerHTML = `<main class="page review-page"><section class="panel empty"><h2>Review unavailable</h2><p class="error" role="alert">${esc(review.error)}</p><button type="button" data-act="review-start">Reload</button></section></main>`;
		return;
	}
	if (review.phase === "empty") {
		root.innerHTML = `<main class="page review-page"><section class="panel empty"><div class="empty-icon">${icon("check", 28)}</div><h2>Nothing due</h2><p>Words you look up resurface here when they are due.</p><a class="button primary" href="#/">Look up a word ${icon("arrow", 16)}</a></section></main>`;
		return;
	}
	if (review.phase === "done") {
		const summary = review.summary || { formal_count: 0, formal_correct: 0, practice_count: 0, reinforce: [] };
		root.innerHTML = `<main class="page review-page"><section class="panel empty"><div class="empty-icon">${icon("check", 28)}</div><h2>Session complete</h2><div class="summary"><div><span>${summary.formal_correct}</span>formal correct</div><div><span>${summary.formal_count}</span>formal answered</div><div><span>${summary.practice_count}</span>practice</div></div>${summary.reinforce && summary.reinforce.length ? `<div class="reinforce">${summary.reinforce.map((item) => `<a class="badge" href="#/?word=${encodeURIComponent(item)}">${esc(item)}</a>`).join("")}</div>` : ""}<button class="primary" type="button" data-act="review-start">Next session</button></section></main>`;
		return;
	}
	const q = review.question;
	const hintLabels = ["First letter", "Other gloss", "Reveal answer"];
	const hintIndex = review.hints.length ? review.hints[review.hints.length - 1].level : 0;
	root.innerHTML = `<main class="page review-page"><div class="session-top"><span><strong>${review.answered}</strong> answered</span><a href="#/">Exit ${icon("close", 14)}</a></div><section class="panel question" aria-busy="${review.busy}"><span class="kind">${q.kind === "ai" ? "Cloze · AI" : "Definition"}${q.practice ? " · practice" : ""}</span><p class="prompt reading">${esc(q.prompt)}</p>${review.error ? `<p class="error" role="alert">${esc(review.error)}</p>` : ""}${review.grade ? `<div class="feedback rise ${review.grade.correct ? "" : "incorrect"}" role="status"><span>${icon(review.grade.correct ? "check" : "book", 20)}</span><div><strong>${esc(review.shown || "")}</strong><p>${esc(gradeCopy(review.grade))}</p></div></div><div class="actions"><span class="muted small">${review.grade.hint_level ? review.grade.hint_level + " hint level" : "No hints"}</span><button class="primary" type="button" data-act="review-next" ${review.busy ? "disabled" : ""}>Next <kbd>Enter</kbd></button></div>` : `<form id="review-form"><label for="review-answer">Which word?</label><input id="review-answer" placeholder="Type the word…" autocomplete="off" autocapitalize="off" spellcheck="false" ${review.busy ? "disabled" : ""}><div class="actions"><button class="ghost" type="button" data-act="review-reveal" ${review.busy || hintIndex >= 3 ? "disabled" : ""}>Reveal</button><button class="primary" type="submit" ${review.busy ? "disabled" : ""}>Check <kbd>Enter</kbd></button></div></form>`}<div class="hint-section"><button class="ghost hint-button" type="button" data-act="review-hint" ${review.busy || review.grade || hintIndex >= 3 ? "disabled" : ""}>${icon("spark", 14)}${hintIndex >= 3 ? "No hints left" : hintLabels[hintIndex] || "Hint"} <span>${Math.min(hintIndex + 1, 3)} / 3</span></button>${review.hints.length ? `<ol class="hints">${review.hints.map((hint) => `<li><span class="hint-step">${hint.level}</span><span class="${hint.level === 1 ? "mask" : ""}">${esc(hint.text || "Not available for this entry.")}</span></li>`).join("")}</ol>` : ""}</div></section></main>`;
	if (!review.grade) document.getElementById("review-answer")?.focus();
}
async function loadNext(token) {
	if (!reviewAlive(token)) return;
	review.busy = true;
	review.grade = null;
	review.shown = "";
	review.hints = [];
	review.error = "";
	paintReview();
	try {
		const next = await api("GET", "/api/review/" + encodeURIComponent(review.session) + "/next");
		if (!reviewAlive(token)) return;
		if (!next.has) {
			if (!review.answered) review.phase = "empty";
			else {
				review.summary = await api("GET", "/api/review/" + encodeURIComponent(review.session) + "/summary");
				if (!reviewAlive(token)) return;
				review.phase = "done";
			}
			review.question = null;
		} else {
			review.phase = "question";
			review.question = next.question;
		}
	} catch (error) {
		if (!reviewAlive(token)) return;
		review.phase = "error";
		review.error = errText(error);
	} finally {
		if (!reviewAlive(token)) return;
		review.busy = false;
		paintReview();
	}
}
async function startReview() {
	const token = ++reviewToken;
	resetReview();
	paintReview();
	try {
		const started = await api("POST", "/api/review/start");
		if (!reviewAlive(token)) return;
		review.session = started.session_id;
		await loadNext(token);
	} catch (error) {
		if (!reviewAlive(token)) return;
		review.phase = "error";
		review.error = errText(error);
		review.busy = false;
		paintReview();
	}
}
async function reviewHint(level) {
	const token = reviewToken;
	if (!review.question || review.grade || review.busy) return;
	review.busy = true;
	review.error = "";
	paintReview();
	try {
		const hint = await api("POST", `/api/review/question/${review.question.id}/hint`, { level });
		if (!reviewAlive(token)) return;
		review.hints = review.hints.filter((item) => item.level !== hint.level).concat(hint).sort((a, b) => a.level - b.level);
	} catch (error) {
		if (!reviewAlive(token)) return;
		review.error = errText(error);
	} finally {
		if (!reviewAlive(token)) return;
		review.busy = false;
		paintReview();
	}
}
async function reviewSubmit(answer) {
	const token = reviewToken;
	if (!review.question || review.grade || review.busy) return;
	const text = answer.trim();
	if (!text) return;
	review.busy = true;
	review.error = "";
	paintReview();
	try {
		const grade = await api("POST", `/api/review/question/${review.question.id}/submit`, { answer: text });
		if (!reviewAlive(token)) return;
		review.grade = grade;
		if (!grade.duplicate) review.answered += 1;
		try {
			const entry = await api("GET", "/api/entry/" + grade.entry_id);
			if (!reviewAlive(token)) return;
			review.shown = entry.word;
		} catch { review.shown = text; }
	} catch (error) {
		if (!reviewAlive(token)) return;
		review.error = errText(error);
	} finally {
		if (!reviewAlive(token)) return;
		review.busy = false;
		paintReview();
	}
}
function chartHtml(days) {
	const max = Math.max(1, ...days.map((day) => day.lookups + day.reviews));
	const lookups = days.reduce((sum, day) => sum + day.lookups, 0);
	const reviews = days.reduce((sum, day) => sum + day.reviews, 0);
	return `<section class="panel activity"><div class="section-heading"><h2>Last 14 days</h2><div class="legend"><span>${lookups} lookups</span><span class="review-key">${reviews} reviews</span></div></div><div class="chart" role="img" aria-label="${lookups} lookups and ${reviews} reviews over the last 14 days.">${days.map((day, index) => {
		const total = day.lookups + day.reviews;
		const height = total ? (total / max) * 100 : 0;
		return `<div class="chart-column" title="${esc(day.date)}: ${day.lookups} lookups, ${day.reviews} reviews"><div class="bar-track"><div class="bar" style="height:${height}%">${total ? `<div class="lookup-bar" style="flex:${day.lookups}"></div><div class="review-bar" style="flex:${day.reviews}"></div>` : ""}</div></div><span>${index % 3 === 0 ? esc(day.date.slice(5)) : "·"}</span></div>`;
	}).join("")}</div>${!lookups && !reviews ? `<p class="muted small">No activity yet.</p>` : ""}</section>`;
}
async function pageStats(gen) {
	content().innerHTML = `<main class="page insights"><h1>Stats</h1><div class="skeleton" role="status"></div></main>`;
	try {
		const stats = await api("GET", "/api/stats");
		if (gen !== viewGen) return;
		const active = stats.stage0 + stats.stage1 + stats.stage2 + stats.stage3;
		const stable = active ? (stats.stage3 / active) * 100 : 0;
		const familiar = active ? ((stats.stage3 + stats.stage2) / active) * 100 : 0;
		content().innerHTML = `<main class="page insights"><h1>Stats</h1><dl class="metric-grid"><div class="metric"><dt>New</dt><dd>${num(stats.stage0)}</dd></div><div class="metric"><dt>Learning</dt><dd>${num(stats.stage1)}</dd></div><div class="metric"><dt>Familiar</dt><dd>${num(stats.stage2)}</dd></div><div class="metric"><dt>Stable</dt><dd>${num(stats.stage3)}</dd></div></dl>${chartHtml(stats.days || [])}<div class="insight-grid"><section class="panel distribution"><div class="section-heading"><h2>Stages</h2></div><div class="distribution-content"><div class="donut" style="--stable:${stable}%;--familiar:${familiar}%"><div><strong>${Math.round(stable)}<small>%</small></strong><span>stable</span></div></div><dl><div><dt><i style="background:var(--line)"></i>New + learning</dt><dd>${num(stats.stage0 + stats.stage1)}</dd></div><div><dt><i style="background:var(--kraft)"></i>Familiar</dt><dd>${num(stats.stage2)}</dd></div><div><dt><i style="background:var(--accent)"></i>Stable</dt><dd>${num(stats.stage3)}</dd></div></dl></div>${stats.paused ? `<p class="muted small">${stats.paused} paused, not counted above.</p>` : ""}</section><section class="panel"><div class="section-heading"><h2>Review</h2><a href="#/review">Start</a></div><p class="muted">Due words are chosen when a session starts. Paused words stay out of the queue.</p><p><a class="button" href="#/library">Open words</a></p></section></div></main>`;
	} catch (error) {
		if (gen !== viewGen) return;
		content().innerHTML = `<main class="page insights"><h1>Stats</h1><p class="error" role="alert">${esc(errText(error))}</p><button type="button" data-act="stats-retry">Retry</button></main>`;
	}
}
function paintImport() {
	const slot = document.getElementById("import-status");
	if (!slot) return;
	const task = slot._task;
	if (!task) { slot.innerHTML = ""; return; }
	if (task.status === "running") {
		slot.innerHTML = `<p class="import-note" role="status">Importing ${esc(task.id)} — added ${task.added}, updated ${task.updated}, skipped ${task.skipped}, failed ${task.failed}.</p>`;
		return;
	}
	if (task.status === "failed") {
		const message = task.error && task.error.message ? task.error.message : "Import failed";
		const code = task.error && task.error.code ? ` (${task.error.code})` : "";
		slot.innerHTML = `<p class="error" role="alert">${esc(message + code)}</p>`;
		return;
	}
	slot.innerHTML = `<p class="import-note" role="status">Imported. Added ${task.added}, updated ${task.updated}, skipped ${task.skipped}, failed ${task.failed}.</p>`;
}
async function watchImport(id) {
	const slot = document.getElementById("import-status");
	for (;;) {
		const task = await api("GET", "/api/import/" + encodeURIComponent(id));
		const live = document.getElementById("import-status");
		if (!live || live !== slot) return;
		live._task = task;
		paintImport();
		if (task.status !== "running") {
			if (task.status === "succeeded") {
				try {
					const status = await api("GET", "/api/status");
					const state = document.getElementById("dict-state");
					const badge = document.getElementById("dict-badge");
					if (state) state.textContent = status.state;
					if (badge) badge.textContent = status.dict_ready ? "ECDICT ready" : "Not imported";
				} catch { /* status line stays */ }
			}
			return;
		}
		await new Promise((resolve) => setTimeout(resolve, 800));
	}
}
async function pageSettings(gen) {
	let path = "";
	try { path = localStorage.getItem("lexica.import-path") || ""; } catch { path = ""; }
	content().innerHTML = `<main class="page settings"><h1>Settings</h1><section class="panel"><div class="setting-row"><h3>Theme</h3><div class="segmented">${["light", "dark", "system"].map((item) => `<button type="button" data-act="pref" data-key="theme" data-value="${item}" class="${prefs.theme === item ? "chosen" : ""}" aria-pressed="${prefs.theme === item}">${item[0].toUpperCase() + item.slice(1)}</button>`).join("")}</div></div><div class="setting-row"><h3>Lookup</h3><div class="segmented"><button type="button" data-act="pref" data-key="reading" data-value="guided" class="${prefs.reading === "guided" ? "chosen" : ""}" aria-pressed="${prefs.reading === "guided"}">Context first</button><button type="button" data-act="pref" data-key="reading" data-value="full" class="${prefs.reading === "full" ? "chosen" : ""}" aria-pressed="${prefs.reading === "full"}">Full entry</button></div></div><div class="setting-row last"><h3>Text size</h3><div class="segmented"><button type="button" data-act="pref" data-key="fontSize" data-value="normal" class="${prefs.fontSize === "normal" ? "chosen" : ""}" aria-pressed="${prefs.fontSize === "normal"}">Normal</button><button type="button" data-act="pref" data-key="fontSize" data-value="large" class="${prefs.fontSize === "large" ? "chosen" : ""}" aria-pressed="${prefs.fontSize === "large"}">Large</button></div></div></section><section class="panel dictionary"><div class="section-heading"><h2>Dictionary</h2></div><div class="dictionary-info"><div><strong id="dict-state">…</strong><span>startup</span></div><span class="badge" id="dict-badge">Loading</span></div><p class="muted small">Import a local ECDICT <code>stardict.db</code> or CSV. Repeat imports keep the same entry ids. <a href="https://github.com/skywind3000/ECDICT/releases/tag/1.0.28" target="_blank" rel="noreferrer">Download page ↗</a></p><form class="import-row" id="import-form"><input id="import-path" value="${esc(path)}" placeholder="D:\\Dictionary\\stardict.db" spellcheck="false" aria-label="Dictionary path"><button class="primary" type="submit">Import</button></form><div id="import-status"></div></section><section class="panel" id="ai-examples"><div class="section-heading"><h2>Example sentences</h2></div><p class="muted small">Optional. The key is read from the <code>LEXICA_AI_KEY</code> environment variable and is never shown here. Address must be https, except localhost.</p><div id="ai-form"><p class="muted small">Loading…</p></div></section><section class="panel shortcuts"><div class="section-heading"><h2>Shortcuts</h2></div><dl><div><dt>Focus search</dt><dd><kbd>Ctrl</kbd><kbd>K</kbd></dd></div><div><dt>Suggestions</dt><dd><kbd>↑</kbd><kbd>↓</kbd><kbd>Enter</kbd></dd></div><div><dt>English / full entry</dt><dd><kbd>2</kbd><kbd>3</kbd></dd></div><div><dt>Submit answer, next item</dt><dd><kbd>Enter</kbd></dd></div></dl></section><p class="about">Lookups, bookmarks, notes and review records stay on this device.</p><div class="credit"><a href="https://github.com/YinLingxiao/Lexica." target="_blank" rel="noreferrer" aria-label="Lexica source"><span class="credit-mark"></span></a></div></main>`;
	try {
		const [status, ai] = await Promise.all([api("GET", "/api/status"), api("GET", "/api/ai/config")]);
		if (gen !== viewGen) return;
		const state = document.getElementById("dict-state");
		const badge = document.getElementById("dict-badge");
		if (state) state.textContent = status.state;
		if (badge) badge.textContent = status.dict_ready ? "ECDICT ready" : "Not imported";
		const form = document.getElementById("ai-form");
		if (form) form.innerHTML = `<div class="setting-row"><h3>Enable</h3><div class="segmented"><button type="button" id="ai-off" data-act="ai-flag" data-value="0" class="${ai.enabled ? "" : "chosen"}" aria-pressed="${!ai.enabled}">Off</button><button type="button" id="ai-on" data-act="ai-flag" data-value="1" class="${ai.enabled ? "chosen" : ""}" aria-pressed="${ai.enabled}">On</button></div></div><div class="ai-fields"><label><span>Service address</span><input id="ai-url" value="${esc(ai.base_url)}" placeholder="https://api.deepseek.com/v1" spellcheck="false"></label><label><span>Model</span><input id="ai-model" value="${esc(ai.model)}" placeholder="deepseek-chat" spellcheck="false"></label><p class="muted small">Key ${ai.key_configured ? "is set in the environment." : "is missing. Set LEXICA_AI_KEY and restart."}</p><div class="ai-actions"><button type="button" data-act="ai-save">Save</button><button class="ghost" type="button" data-act="ai-test">Test connection</button></div><p id="ai-msg" class="import-note" role="status"></p></div>`;
	} catch (error) {
		if (gen !== viewGen) return;
		const state = document.getElementById("dict-state");
		if (state) state.textContent = "Error";
		const form = document.getElementById("ai-form");
		if (form) form.innerHTML = `<p class="error" role="alert">${esc(errText(error))}</p>`;
	}
}
function render() {
	const gen = ++viewGen;
	const { path, params } = route();
	updateChrome(path);
	if (path === "/library") return pageLibrary();
	if (path === "/review") return startReview();
	if (path === "/stats") return pageStats(gen);
	if (path === "/settings") return pageSettings(gen);
	return pageSearch(gen, params.get("word") || "");
}
function choose(button) {
	button.parentElement.querySelectorAll("button").forEach((item) => {
		const on = item === button;
		item.classList.toggle("chosen", on);
		item.setAttribute("aria-pressed", on ? "true" : "false");
	});
}
async function onClick(event) {
	const button = event.target.closest("[data-act]");
	if (!button) return;
	const act = button.dataset.act;
	if (act === "clear-search") {
		const input = document.querySelector("[data-search]");
		if (!input) return;
		input.value = "";
		onSearchInput(input);
		input.focus();
	} else if (act === "commit-search") {
		commitSearch(document.querySelector("[data-search]")?.value || "");
	} else if (act === "pick" || act === "lookup") {
		commitSearch(button.dataset.word || "");
	} else if (act === "english") {
		word.showEnglish = true;
		paintWord();
	} else if (act === "full") {
		word.showEnglish = true;
		word.showFull = true;
		paintWord();
	} else if (act === "fav") {
		saveVocab("favorite", { value: !(word.vocab && word.vocab.favorite) });
	} else if (act === "pause") {
		saveVocab("paused", { value: !(word.vocab && word.vocab.paused) });
	} else if (act === "save-note") {
		saveVocab("note", { text: document.getElementById("note")?.value || "" });
	} else if (act === "copy" && word.entry) {
		const entry = word.entry;
		const text = [entry.word, entry.phonetic, entry.definition, entry.translation].filter(Boolean).join("\n");
		try {
			await navigator.clipboard.writeText(text);
			word.message = "Entry copied";
			word.error = "";
		} catch {
			word.error = "Clipboard unavailable.";
		}
		paintWord();
	} else if (act === "speak" && word.entry && window.speechSynthesis) {
		const utterance = new SpeechSynthesisUtterance(word.entry.word);
		utterance.lang = "en-US";
		utterance.rate = 0.85;
		speechSynthesis.cancel();
		speechSynthesis.speak(utterance);
	} else if (act === "ai") {
		generateExample();
	} else if (act === "lib-filter") {
		lib.filter = button.dataset.filter;
		lib.page = 1;
		document.querySelectorAll("[data-act='lib-filter']").forEach((item) => {
			const on = item === button;
			item.classList.toggle("chosen", on);
			item.setAttribute("aria-pressed", on ? "true" : "false");
		});
		reloadLibrary();
	} else if (act === "lib-clear") {
		lib.q = "";
		lib.filter = "all";
		lib.page = 1;
		pageLibrary();
	} else if (act === "lib-prev") {
		lib.page = Math.max(1, lib.page - 1);
		reloadLibrary();
	} else if (act === "lib-next") {
		lib.page += 1;
		reloadLibrary();
	} else if (act === "lib-retry") {
		reloadLibrary();
	} else if (act === "resume") {
		button.disabled = true;
		try {
			await api("PUT", `/api/vocab/${button.dataset.id}/paused`, { value: false });
			reloadLibrary();
		} catch (error) {
			button.disabled = false;
			const body = document.getElementById("lib-body");
			if (body) body.insertAdjacentHTML("afterbegin", `<p class="error" role="alert">${esc(errText(error))}</p>`);
		}
	} else if (act === "review-start") {
		startReview();
	} else if (act === "review-hint") {
		const level = review.hints.length ? review.hints[review.hints.length - 1].level + 1 : 1;
		reviewHint(level);
	} else if (act === "review-reveal") {
		reviewHint(3);
	} else if (act === "review-next") {
		loadNext(reviewToken);
	} else if (act === "stats-retry") {
		pageStats(++viewGen);
	} else if (act === "pref") {
		choose(button);
		prefs[button.dataset.key] = button.dataset.value;
		applyPrefs();
	} else if (act === "ai-flag") {
		choose(button);
	} else if (act === "ai-save" || act === "ai-test") {
		const msg = document.getElementById("ai-msg");
		const enabled = document.getElementById("ai-on")?.classList.contains("chosen");
		const base = document.getElementById("ai-url")?.value || "";
		const model = document.getElementById("ai-model")?.value || "";
		button.disabled = true;
		try {
			if (act === "ai-save") {
				await api("PUT", "/api/ai/config", { enabled: !!enabled, base_url: base, model });
				if (msg) msg.textContent = "AI settings saved.";
			} else {
				await api("POST", "/api/ai/test");
				if (msg) msg.textContent = "Connection OK.";
			}
		} catch (error) {
			if (msg) msg.textContent = errText(error);
		} finally {
			button.disabled = false;
		}
	}
}
function onSubmit(event) {
	if (event.target.id === "review-form") {
		event.preventDefault();
		reviewSubmit(document.getElementById("review-answer")?.value || "");
	} else if (event.target.id === "import-form") {
		event.preventDefault();
		const input = document.getElementById("import-path");
		const path = input ? input.value.trim() : "";
		if (!path) return;
		try { localStorage.setItem("lexica.import-path", path); } catch { /* ignore */ }
		const slot = document.getElementById("import-status");
		api("POST", "/api/import", { path }).then((task) => {
			if (slot) { slot._task = task; paintImport(); }
			if (task.status === "running") watchImport(task.id);
		}).catch((error) => {
			if (slot) slot.innerHTML = `<p class="error" role="alert">${esc(errText(error))}</p>`;
		});
	}
}
function onInput(event) {
	if (event.target.matches("[data-search]")) onSearchInput(event.target);
	else if (event.target.id === "lib-q") {
		lib.q = event.target.value;
		lib.page = 1;
		clearTimeout(lib.timer);
		lib.timer = setTimeout(reloadLibrary, 150);
	} else if (event.target.id === "note") {
		const count = document.getElementById("note-count");
		if (count) count.textContent = event.target.value.length + " / 5000";
	}
}
function onKey(event) {
	if (event.target.matches?.("[data-search]")) return onSearchKey(event);
	const typing = event.target.matches?.("input,textarea,select");
	if ((event.key === "/" && !typing) || ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k")) {
		event.preventDefault();
		if (route().path !== "/" || route().params.get("word")) {
			focusSearch = true;
			go("/");
		} else document.querySelector("[data-search]")?.focus();
		return;
	}
	if (typing || event.ctrlKey || event.metaKey || event.altKey) return;
	if (route().path === "/" && word.entry && route().params.get("word")) {
		if (event.key === "2") { word.showEnglish = true; paintWord(); }
		if (event.key === "3") { word.showEnglish = true; word.showFull = true; paintWord(); }
	}
	if (event.key === "Enter" && route().path === "/review" && review.grade && !review.busy) {
		event.preventDefault();
		loadNext();
	}
}
document.getElementById("collapse").addEventListener("click", () => {
	document.getElementById("shell").classList.add("sidebar-animated");
	const sidebar = document.getElementById("sidebar");
	const collapsed = !sidebar.classList.contains("collapsed");
	sidebar.classList.toggle("collapsed", collapsed);
	document.getElementById("workspace").classList.toggle("sidebar-collapsed", collapsed);
	sidebar.querySelector(".collapse-toggle").setAttribute("aria-expanded", collapsed ? "false" : "true");
	sidebar.querySelector(".collapse-toggle").setAttribute("aria-label", collapsed ? "Expand sidebar" : "Collapse sidebar");
	try { localStorage.setItem("lexica.sidebar-collapsed", collapsed ? "1" : "0"); } catch { /* ignore */ }
});
document.getElementById("theme").addEventListener("click", () => {
	const dark = prefs.theme === "dark" || (prefs.theme === "system" && matchMedia("(prefers-color-scheme: dark)").matches);
	prefs.theme = dark ? "light" : "dark";
	applyPrefs();
});
document.addEventListener("click", onClick);
document.addEventListener("submit", onSubmit);
document.addEventListener("input", onInput);
document.addEventListener("keydown", onKey);
window.addEventListener("hashchange", render);
loadPrefs();
try {
	if (localStorage.getItem("lexica.sidebar-collapsed") === "1") {
		document.getElementById("sidebar").classList.add("collapsed");
		document.getElementById("workspace").classList.add("sidebar-collapsed");
	}
} catch { /* expanded */ }
applyPrefs();
render();
