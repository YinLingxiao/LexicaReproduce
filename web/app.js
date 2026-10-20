// Presentation only: dictionary, grading, scheduling and persistence stay in C++.
const $ = (selector, root = document) => root.querySelector(selector);
const app = $('#app');
const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const titles = {lookup:'探索单词',vocab:'我的词汇',review:'我的复习',stats:'学习足迹',settings:'偏好与设置'};
const stages = ['刚刚认识','有点熟悉','很深印象','熟练掌握'];
let page = '', generation = 0, ready = false, currentEntry = null, session = null, question = null, graded = false;
let noteDraft = '', noteSaved = '', toastTimer, lookupSequence = 0, importTaskId = '';
const loading = '<div class="loading" role="status">正在读取你的学习空间…</div>';
const errorBox = error => `<div class="error" role="alert">${escape(error.message || error)}</div>`;
const empty = (title, text) => `<div class="empty"><div class="empty-symbol">✧</div><h2>${escape(title)}</h2><p>${escape(text)}</p></div>`;
function toast(message) { const box = $('#toast'); box.textContent = message; box.classList.add('visible'); clearTimeout(toastTimer); toastTimer = setTimeout(() => box.classList.remove('visible'), 3500); }
async function api(path, method = 'GET', body) {
  let response;
  try { response = await fetch(`/api${path}`, {method, headers: body === undefined ? {} : {'Content-Type':'application/json'}, body: body === undefined ? undefined : JSON.stringify(body), signal:AbortSignal.timeout(25000)}); }
  catch { throw new Error('本地服务未响应，请确认 Lexica 程序仍在运行，然后重试。'); }
  let result;
  try { result = await response.json(); } catch { throw new Error('服务响应格式不正确，请检查是否打开了正确的本地地址。'); }
  if (!response.ok || !result.ok) { const error = new Error(result.error?.message || `请求失败（${response.status}）`); error.code = result.error?.code; throw error; }
  return result.data;
}
async function busy(button, action) {
  if (button?.disabled) return;
  if (button) button.disabled = true;
  try { await action(); } catch (error) { toast(error.message); }
  finally { if (button) button.disabled = false; }
}
function heading(kicker, title, subtitle) { return `<div class="page-heading"><div><div class="eyebrow">${kicker}</div><h1>${title}</h1><p class="subtitle">${subtitle}</p></div><div class="date-label">${new Date().toLocaleDateString('zh-CN',{month:'long',day:'numeric',weekday:'long'})}</div></div>`; }
function allowDiscard() { return noteDraft === noteSaved || confirm('笔记尚未保存，确定离开并放弃修改吗？'); }
function applyReadingSize(value) { document.documentElement.style.setProperty('--reading-size', `${[14,15,17,19].includes(Number(value)) ? Number(value) : 15}px`); }
try { applyReadingSize(localStorage.getItem('lexica-reading-size')); } catch { applyReadingSize(15); }
async function refreshStatus() {
  try { const status = await api('/status'); ready = status.dict_ready; $('#service-status').textContent = ready ? '本地服务已连接' : '已连接 · 待导入词库'; $('#status-dot').classList.add('online'); }
  catch { ready = false; $('#service-status').textContent = '本地服务未连接'; $('#status-dot').classList.remove('online'); }
}
function lookupLayout() {
  app.innerHTML = heading('THE WORLD, ONE WORD AT A TIME', '今天的你，<em>背单词了吗？</em>', '输入一个英文单词，看看它的意思。') +
    `<div id="dict-banner"></div><div class="search-wrap"><form id="search-form" class="search-form"><span class="search-icon" aria-hidden="true">⌕</span><input id="search-input" aria-label="输入英文单词" role="combobox" aria-autocomplete="list" aria-expanded="false" aria-controls="suggestions" autocomplete="off" placeholder="遇见了什么新单词？" spellcheck="false"><button class="primary" type="submit">查词 <span aria-hidden="true">↗</span></button></form><div id="suggestions" class="suggestions" role="listbox" hidden></div></div><div class="search-caption"><kbd>Enter</kbd> 查询单词 <span>·</span> <kbd>↑</kbd><kbd>↓</kbd> 选择建议 <span>·</span> 自动补全单词</div>
    <div class="lookup-grid"><div><section id="word-panel" class="card word-card"></section><section id="note-panel" class="card note-card" hidden></section></div><aside class="side-stack"><section class="card side-card"><div class="side-title"><h2>最近遇见</h2><button class="text-button" id="history-more">更多 ↗</button></div><div id="history">${loading}</div></section><section class="tip-card"><span class="tiny-label">MAKE IT STICK</span><div class="tip-title">Small steps. Big worlds.</div><p></p><a href="#/review" class="text-button">去温习一下 <span aria-hidden="true">→</span></a></section></aside></div>`;
  $('#word-panel').innerHTML = `<div class="card-label">YOUR NEXT DISCOVERY <span class="tag">坚持看一点</span></div><div class="empty"><div class="welcome-word">Hello, curious mind.</div><p class="welcome-text">${['apple','banana','run'].map(word => `<button data-quick="${word}">${word} ↗</button>`).join('')}</div></div>`;
  document.querySelectorAll('[data-quick]').forEach(button => button.onclick = () => { $('#search-input').value = button.dataset.quick; lookup(button.dataset.quick); });
  const input = $('#search-input'), list = $('#suggestions');
  let timer, sequence = 0, candidates = [], selected = -1;
  const close = () => { sequence++; clearTimeout(timer); list.hidden = true; input.setAttribute('aria-expanded','false'); input.removeAttribute('aria-activedescendant'); candidates = []; selected = -1; };
  const selectWord = word => { input.value = word; close(); lookup(word); };
  input.oninput = () => {
    close(); const term = input.value.trim(), request = sequence;
    if (!term) return;
    timer = setTimeout(async () => {
      try {
        const result = await api(`/suggest?q=${encodeURIComponent(term)}`);
        if (sequence !== request || !input.isConnected) return;
        candidates = result; list.innerHTML = result.map((item,i) => `<button type="button" role="option" aria-selected="false" id="suggest-${i}" data-index="${i}">${escape(item.word)}</button>`).join('');
        list.hidden = !result.length; input.setAttribute('aria-expanded', String(!!result.length));
        list.querySelectorAll('button').forEach(button => button.onclick = () => selectWord(candidates[Number(button.dataset.index)].word));
      } catch { close(); }
    },180);
  };
  input.onkeydown = event => {
    if (event.key === 'Escape') close();
    if (['ArrowDown','ArrowUp'].includes(event.key) && candidates.length) {
      event.preventDefault(); selected = (selected + (event.key === 'ArrowDown' ? 1 : -1) + candidates.length) % candidates.length;
      list.querySelectorAll('button').forEach((button,i) => { button.classList.toggle('selected', i === selected); button.setAttribute('aria-selected',String(i === selected)); }); input.setAttribute('aria-activedescendant',`suggest-${selected}`);
    }
    if (event.key === 'Enter' && selected >= 0) { event.preventDefault(); selectWord(candidates[selected].word); }
  };
  $('#search-form').onsubmit = event => { event.preventDefault(); close(); lookup(input.value); };
  input.onblur = () => setTimeout(() => { if (!list.contains(document.activeElement)) close(); },150);
  let historySize = 5;
  $('#history-more').onclick = () => { historySize = historySize === 5 ? 20 : 5; $('#history-more').textContent = historySize === 5 ? '更多 ↗' : '收起 ↑'; renderHistory(historySize); };
  renderHistory();
}
async function renderHistory(size = 5) {
  const target = $('#history'); if (!target) return;
  try {
    const data = await api('/history?page=1&size=100');
    if (!target.isConnected) return;
    data.items = data.items.filter((item, i, all) => all.findIndex(x => x.id === item.id) === i).slice(0, size);
    target.innerHTML = data.items.length ? data.items.map(item => `<button class="history-item" data-id="${item.id}"><span>${escape(item.word)}</span><span>↗</span></button>`).join('') : '<p class="subtitle">还没有查词记录。<br>从第一个单词开始吧。</p>';
    target.querySelectorAll('button').forEach(button => button.onclick = () => loadEntry(button.dataset.id));
  } catch (error) { if (target.isConnected) target.innerHTML = errorBox(error); }
}
async function personalState(entry) {
  // The current API exposes personal state only through a paginated list.
  for (let p = 1; ; p++) {
    const data = await api(`/vocab?q=${encodeURIComponent(entry.word)}&page=${p}&size=100`);
    const found = data.items.find(item => item.id === entry.id);
    if (found) return found;
    if (p * 100 >= data.total) return {favorite:false,note:'',paused:false,stage:0};
  }
}
async function lookup(word) {
  word = word.trim(); if (!word) return $('#search-input')?.focus();
  if (!allowDiscard()) return;
  const seq = ++lookupSequence, target = $('#word-panel');
  noteDraft = noteSaved = ''; currentEntry = null; target.innerHTML = loading; $('#note-panel').hidden = true;
  try { const entry = await api('/lookup','POST',{word,op_id:crypto.randomUUID()}); if (seq !== lookupSequence || !target.isConnected) return; await showEntry(entry, target, seq); renderHistory(); }
  catch (error) { if (seq === lookupSequence && target.isConnected) target.innerHTML = error.code === 'WORD_NOT_FOUND' ? empty('还没有找到这个词', '检查一下拼写，或试试输入它的原形。') : errorBox(error); }
}
async function loadEntry(id) {
  if (!allowDiscard()) return;
  noteDraft = noteSaved = '';
  if (page !== 'lookup') { location.hash = '/lookup'; await route(); }
  const seq = ++lookupSequence, target = $('#word-panel'); target.innerHTML = loading; $('#note-panel').hidden = true;
  try { const entry = await api(`/entry/${id}`); if (seq !== lookupSequence || !target.isConnected) return; $('#search-input').value = entry.word; await showEntry(entry,target,seq); }
  catch (error) { if (seq === lookupSequence && target.isConnected) target.innerHTML = errorBox(error); }
}
async function showEntry(entry, target, seq) {
  let personal, stateError;
  try { personal = await personalState(entry); } catch (error) { stateError = error; }
  if (seq !== lookupSequence || !target.isConnected) return;
  currentEntry = entry;
  target.innerHTML = `<div class="card-label">A WORD TO KEEP <span class="tag">${personal ? stages[personal.stage] || stages[0] : '词条详情'}</span></div><div class="word-top"><h2 class="word-title">${escape(entry.word)}</h2><button id="favorite" class="favorite ${personal?.favorite ? 'on':''}" aria-label="${personal?.favorite ? '取消收藏':'收藏单词'}" aria-pressed="${!!personal?.favorite}" ${stateError ? 'disabled':''}>${personal?.favorite ? '★':'☆'}</button></div><div class="phonetic">${entry.phonetic ? `/ ${escape(entry.phonetic)} /` : '暂无音标'}${entry.query && entry.query !== entry.word ? ` · 由 ${escape(entry.query)} 还原` : ''}</div><div class="definition-section"><div class="section-label">中文释义 / TRANSLATION</div><p class="meaning">${escape(entry.translation || '词库暂未提供中文释义。')}</p></div><div class="definition-section"><div class="section-label">英文释义 / DEFINITION</div><p class="meaning english">${escape(entry.definition || 'No English definition available.')}</p></div><div class="definition-section"><div class="side-title"><span class="section-label">语境灵感 / AI EXAMPLE</span><button class="text-button" id="generate">生成例句 ↗</button></div><div id="example" class="subtitle">用一个句子，认识单词的另一面。需在设置中启用 AI。</div></div>${stateError ? errorBox(new Error('个人词汇状态读取失败，请重新打开词条后再编辑。')) : ''}`;
  const fav = $('#favorite');
  fav.onclick = () => busy(fav, async () => { const value = !personal.favorite; await api(`/vocab/${entry.id}/favorite`,'PUT',{value}); personal.favorite = value; fav.classList.toggle('on',value); fav.textContent = value ? '★':'☆'; fav.setAttribute('aria-pressed',String(value)); fav.setAttribute('aria-label',value ? '取消收藏':'收藏单词'); toast(value ? '已收藏，让这个词留下来。' : '已取消收藏'); });
  $('#generate').onclick = event => busy(event.currentTarget, async () => {
    const box = $('#example'); box.textContent = '正在生成例句…';
    try {
      let task = await api('/ai/example','POST',{id:entry.id,word:entry.word});
      for (let count = 0; ['pending','running'].includes(task.status) && count < 45; count++) {
        await new Promise(resolve => setTimeout(resolve,800)); if (!box.isConnected) return; task = await api(`/ai/task/${task.id}`);
      }
      if (!box.isConnected) return;
      box.textContent = task.status === 'succeeded' ? `AI 生成 · ${task.text}` : '例句未生成成功。请检查 AI 设置后重试。';
    } catch (error) { if (box.isConnected) box.textContent = error.message; }
  });
  if (stateError) return;
  noteDraft = noteSaved = personal.note;
  const notes = $('#note-panel'); notes.hidden = false;
  notes.innerHTML = `<div class="side-title"><h2>自己的想法</h2><span class="section-label">MY NOTES</span></div><textarea id="note" aria-label="单词笔记" placeholder="在什么地方遇见它？写下你的理解，或一个属于自己的句子。">${escape(personal.note)}</textarea><div class="note-actions"><span id="note-count">${Array.from(personal.note).length} / 5000 字</span><button id="save-note" class="secondary">保存笔记</button></div>`;
  $('#note').oninput = event => { noteDraft = event.target.value; $('#note-count').textContent = `${Array.from(noteDraft).length} / 5000 字 · 未保存`; };
  $('#save-note').onclick = event => busy(event.currentTarget, async () => {
    const text = $('#note').value; if (Array.from(text).length > 5000) throw new Error('笔记最多 5000 字，当前内容已保留，请精简后保存。');
    await api(`/vocab/${entry.id}/note`,'PUT',{text}); if (currentEntry?.id === entry.id && notes.isConnected) { noteSaved = text; $('#note-count').textContent = `${Array.from(noteDraft).length} / 5000 字${noteDraft === text ? ' · 已保存':' · 未保存'}`; } toast('笔记已保存');
  });
}
function vocabPage() {
  app.innerHTML = heading('YOUR GROWING COLLECTION','你查过这些词语','') + `<form id="filters" class="toolbar"><input name="q" aria-label="搜索我的词汇" placeholder="搜索你的词汇…"><select name="fav" aria-label="收藏筛选"><option value="">全部词汇</option><option value="1">仅看收藏</option><option value="0">未收藏</option></select><select name="stage" aria-label="学习阶段"><option value="">所有阶段</option>${stages.map((name,i)=>`<option value="${i}">${name}</option>`).join('')}</select><select name="paused" aria-label="复习状态"><option value="">全部状态</option><option value="0">学习中</option><option value="1">已暂停</option></select><button class="primary">筛选</button></form><div id="vocab-list" class="card"></div><div id="pagination" class="pagination"></div>`;
  let pageNo = 1, sequence = 0;
  const form = $('#filters'), list = $('#vocab-list'), pagination = $('#pagination');
  async function refresh() {
    const seq = ++sequence; list.innerHTML = loading;
    const params = new URLSearchParams(new FormData(form)); params.set('page',pageNo); params.set('size',12);
    try {
      const data = await api(`/vocab?${params}`); if (seq !== sequence || !list.isConnected) return;
      if (!data.items.length && pageNo > 1) { pageNo--; return refresh(); }
      list.innerHTML = data.items.length ? data.items.map(item => `<div class="vocab-row"><button class="vocab-word" data-open="${item.id}">${escape(item.word)}</button><span class="row-note">${escape(item.note || '还没有笔记')}</span><span class="tag">${stages[item.stage] || stages[0]}</span><button class="favorite ${item.favorite ? 'on':''}" data-fav="${item.id}" aria-label="${item.favorite ? '取消收藏':'收藏'} ${escape(item.word)}">${item.favorite ? '★':'☆'}</button><button class="text-button" data-pause="${item.id}">${item.paused ? '恢复复习':'暂停复习'}</button></div>`).join('') : empty('这里还很安静','试着查询一个新词，或调整筛选条件。');
      list.querySelectorAll('[data-open]').forEach(button => button.onclick = () => loadEntry(button.dataset.open));
      list.querySelectorAll('[data-fav]').forEach(button => button.onclick = () => busy(button,async()=>{ const item = data.items.find(x=>x.id===Number(button.dataset.fav)); await api(`/vocab/${item.id}/favorite`,'PUT',{value:!item.favorite}); await refresh(); }));
      list.querySelectorAll('[data-pause]').forEach(button => button.onclick = () => busy(button,async()=>{ const item = data.items.find(x=>x.id===Number(button.dataset.pause)); await api(`/vocab/${item.id}/paused`,'PUT',{value:!item.paused}); await refresh(); }));
      pagination.innerHTML = `<span>共 ${data.total} 个词 · 第 ${pageNo} / ${Math.max(1,Math.ceil(data.total/12))} 页</span><div><button class="secondary" id="prev" ${pageNo===1?'disabled':''}>上一页</button><button class="secondary" id="next" ${pageNo*12>=data.total?'disabled':''}>下一页</button></div>`;
      $('#prev').onclick = () => { pageNo--; refresh(); }; $('#next').onclick = () => { pageNo++; refresh(); };
    } catch (error) { if (seq === sequence && list.isConnected) { list.innerHTML = errorBox(error); pagination.innerHTML = ''; } }
  }
  form.onsubmit = event => { event.preventDefault(); pageNo = 1; refresh(); };
  form.querySelectorAll('select').forEach(select => select.onchange = () => { pageNo = 1; refresh(); }); refresh();
}
async function statsPage(token) {
  app.innerHTML = heading('EVERY LITTLE EFFORT COUNTS','看见一点点的进步。','不必急着抵达。每一次查阅与温习，都算数。') + `<div id="stats-content">${loading}</div>`;
  try {
    const data = await api('/stats'); if (token !== generation) return;
    const sum = field => data.days.reduce((total,day)=>total+day[field],0), total = data.stage0+data.stage1+data.stage2+data.stage3, peak = Math.max(1,...data.days.flatMap(day=>[day.lookups,day.reviews]));
    $('#stats-content').innerHTML = `<div class="metrics">${[['学习中的词',total,''],['近 14 天查词',sum('lookups'),''],['近 14 天复习',sum('reviews'),''],['暂停复习',data.paused,'']].map(([label,value,note])=>`<div class="card metric"><span>${label}</span><strong>${value}</strong><span>${note}</span></div>`).join('')}</div><section class="card chart-card"><div class="side-title"><h2>学习的节奏</h2><span class="tag">最近 14 天</span></div><p class="subtitle">每天一点点，让语言成为习惯。</p><div class="chart" role="img" aria-label="最近14天查词与正式复习次数">${data.days.map(day=>`<div class="chart-column" title="${day.date}：查词 ${day.lookups} 次，复习 ${day.reviews} 次"><div class="bars"><div class="bar" style="height:${day.lookups/peak*100}%"></div><div class="bar review" style="height:${day.reviews/peak*100}%"></div></div><small>${escape(day.date.slice(5).replace('-','/'))}</small></div>`).join('')}</div><p class="legend">● 查词次数　 <span style="color:#a2b482">●</span> 复习次数</p><details><summary class="text-button">查看每日数据</summary>${data.days.map(day=>`<p class="subtitle">${escape(day.date)} · 查词 ${day.lookups} · 复习 ${day.reviews}</p>`).join('')}</details></section><section class="card chart-card"><h2>你认识的单词们：</h2><div class="stage-bars">${stages.map((name,i)=>`<div class="stage">0${i+1} / ${name}<strong>${data['stage'+i]} <small>词</small></strong></div>`).join('')}</div></section>`;
  } catch(error) { if (token === generation) $('#stats-content').innerHTML = errorBox(error); }
}
function reviewPage() {
  app.innerHTML = heading('A MOMENT TO REMEMBER','复习你的单词','') + '<section id="review-panel" class="card review-card"></section>';
  if (session) { nextQuestion(); return; }
  $('#review-panel').innerHTML = `<div class="review-orbit" aria-hidden="true">↻</div><div class="eyebrow">LESS, BUT BETTER</div><h2>留几分钟，温习一下。</h2><p class="subtitle" style="margin:18px 0">每轮最多 12 道正式题，错词会重复一次。<br>新查的词会在一天后进入首次复习。</p><button id="start-review" class="primary">开始今日复习 →</button><p class="subtitle" style="margin-top:20px"></p>`;
  $('#start-review').onclick = event => busy(event.currentTarget,async()=>{ const result = await api('/review/start','POST',{}); session = result.session_id; await nextQuestion(); });
}
async function nextQuestion() {
  const panel = $('#review-panel'); if (!panel) return; panel.innerHTML = loading;
  try {
    const data = await api(`/review/${session}/next`); if (!panel.isConnected) return;
    if (!data.has) {
      const summary = await api(`/review/${session}/summary`); if (!panel.isConnected) return; session = null; question = null;
      panel.innerHTML = summary.formal_count ? `<div class="review-orbit">✓</div><h2>又向前走了一小步。</h2><p class="subtitle" style="margin:20px 0">正式答对 ${summary.formal_correct} / ${summary.formal_count} 题 · 正确率 ${Math.round(summary.formal_correct/summary.formal_count*100)}%<br>错词重练 ${summary.practice_count} 次</p>${summary.reinforce.length ? `<p class="hint-text">还值得再看一眼：${summary.reinforce.map(escape).join('、')}</p>`:''}<a class="primary" href="#/vocab">看看我的词汇 →</a>` : `${empty('今天先到这里','现在没有到期词。新查的词将在一天后安排首次复习。')}<a href="#/lookup" class="primary">去遇见新单词 →</a>`; return;
    }
    question = data.question; graded = false; const q = question;
    panel.innerHTML = `<div class="review-meta"><span>${q.practice ? '错词重练 · 不影响正式成绩':'正式复习'}</span><span>${q.kind==='ai'?'AI 例句填空':'释义联想'}</span></div><p class="prompt">${escape(q.prompt)}</p><p class="subtitle">根据提示，写下你想到的英文单词。</p><form id="answer-form" class="answer-form"><input id="answer" aria-label="你的答案" placeholder="Your answer…" autocomplete="off" spellcheck="false"><button class="primary">确认答案 →</button></form><button id="hint" class="text-button">需要一点提示？（0 / 3）</button><div id="hint-result" aria-live="polite"></div><div id="grade-result" aria-live="polite"></div>`;
    $('#answer').focus();
    $('#hint').onclick = event => busy(event.currentTarget,async()=>{ const hint = await api(`/review/question/${q.id}/hint`,'POST',{}); if (!panel.isConnected || question!==q) return; $('#hint-result').innerHTML = `<p class="hint-text">${escape(hint.text || '暂无补充释义')}${hint.level===3?'<br><small>已查看答案，本题将按未掌握记录。</small>':''}</p>`; $('#hint').textContent = `再给一点提示（${hint.level} / 3）`; if (hint.level===3) $('#hint').hidden = true; });
    $('#answer-form').onsubmit = event => {
      event.preventDefault(); if (graded) return;
      const answer = $('#answer').value.trim(); if (!answer) { toast('先写下你的答案吧。'); return; }
      busy($('#answer-form button'),async()=>{
        const hintButton = $('#hint'); hintButton.disabled = true;
        try {
          const grade = await api(`/review/question/${q.id}/submit`,'POST',{answer});
          if (!panel.isConnected || question!==q) return; graded = true; $('#answer').disabled = true; $('#answer-form button').hidden = true; hintButton.hidden = true;
          const result = $('#grade-result'); result.innerHTML = `<div class="${grade.correct?'success':'error'}">${grade.correct?'记住了，做得很好。':'没关系，再遇见一次就好。'}<br><small>${grade.practice?'本题为重练，不改变阶段。':`当前阶段：${stages[grade.stage] || stages[0]}`}</small></div><button id="next-question" class="primary">继续 →</button>`;
          $('#next-question').onclick = () => nextQuestion();
          if (!grade.correct) { const entry = await api(`/entry/${q.entry_id}`); if (result.isConnected) { const answerText = document.createElement('p'); answerText.className='hint-text'; answerText.textContent = `正确单词：${entry.word}`; result.prepend(answerText); } }
        } finally { hintButton.disabled = false; }
      });
    };
  } catch (error) { if (panel.isConnected) { if (error.code==='QUESTION_EXPIRED') session = null; panel.innerHTML = errorBox(error)+'<button id="retry-review" class="secondary" style="margin-top:20px">重新读取</button>'; $('#retry-review').onclick = () => reviewPage(); } }
}
async function settingsPage(token) {
  app.innerHTML = heading('MAKE YOURSELF AT HOME','自由调整','管理本地词库。') + `<div class="settings-grid"><section class="card settings-card"><h2>词库与数据</h2><p>导入 ECDICT 的 CSV 或 SQLite 词库。文件由本机 C++ 服务读取，不会上传到网络。</p><form id="import-form"><label class="field">词库文件的完整路径<input id="import-path" required placeholder="D:/dictionaries/ecdict.csv"><small>支持 .csv / .db，请粘贴本机文件路径。</small></label><button class="primary" id="import-button">导入词库 ↗</button></form><div class="task-status" id="import-status" role="status"></div></section><section class="card settings-card"><h2>阅读偏好</h2><p>让文字保持舒适的距离。偏好仅保存在当前浏览器，不影响学习数据。</p><label class="field">释义字号<select id="reading-size"><option value="14">紧凑 · 14px</option><option value="15">标准 · 15px</option><option value="17">舒适 · 17px</option><option value="19">大字 · 19px</option></select></label><p class="meaning english">Words open windows to new worlds.</p></section><section class="card settings-card"><h2>AI 语境灵感</h2><p>可选增强功能。开启后可主动生成例句；关闭时仍可正常查词与复习。</p><div id="ai-config">${loading}</div></section><section class="card settings-card"><h2>你的数据，留在本地</h2><p>词库、收藏、笔记与学习记录由 C++ 服务保存在本机 SQLite 数据库中。</p><div class="banner">备份前请先关闭程序，再复制完整数据目录，包括可能存在的 WAL / SHM 文件。</div><p>默认数据目录在 exe 旁边的 data 文件夹；如果通过 --data-dir 启动，则使用指定目录。</p><span class="tag">LOCAL FIRST · NO ACCOUNT NEEDED</span></section></div>`;
  try { $('#reading-size').value = localStorage.getItem('lexica-reading-size') || '15'; } catch {}
  $('#reading-size').onchange = event => { applyReadingSize(event.target.value); try { localStorage.setItem('lexica-reading-size',event.target.value); toast('阅读偏好已保存'); } catch { toast('字号已应用，但浏览器不允许保存偏好。'); } };
  const importForm = $('#import-form');
  async function pollImport(id) {
    const target = $('#import-status'), button = $('#import-button'); button.disabled = true;
    try {
      while (target.isConnected) {
        const task = await api(`/import/${id}`); if (!target.isConnected) return;
        target.textContent = `${task.status==='running'?'正在导入':task.status==='succeeded'?'导入完成':'导入失败'} · 新增 ${task.added} / 更新 ${task.updated} / 跳过 ${task.skipped} / 失败 ${task.failed}${task.error ? ' · '+task.error.message : ''}`;
        if (task.status!=='running') { importTaskId = ''; await refreshStatus(); break; }
        await new Promise(resolve=>setTimeout(resolve,1000));
      }
    } catch (error) { if (target.isConnected) target.innerHTML = errorBox(error); }
    finally { button.disabled = false; }
  }
  importForm.onsubmit = event => { event.preventDefault(); busy($('#import-button'),async()=>{ const task = await api('/import','POST',{path:$('#import-path').value.trim().replace(/^"|"$/g,'')}); importTaskId=task.id; await pollImport(task.id); }); };
  if (importTaskId) pollImport(importTaskId);
  try {
    const config = await api('/ai/config'); if (token!==generation) return;
    $('#ai-config').innerHTML = `<form id="ai-form"><label class="check-label"><input type="checkbox" id="ai-enabled" ${config.enabled?'checked':''}>启用 AI 例句</label><label class="field">服务地址<input type="url" id="ai-url" value="${escape(config.base_url)}" placeholder="https://api.example.com/v1"></label><label class="field">模型名称<input id="ai-model" value="${escape(config.model)}" placeholder="模型 ID"></label><div class="banner">${config.key_configured?'密钥已由本地环境变量配置。':'尚未配置密钥。请设置 LEXICA_AI_KEY 环境变量后重启程序。'}<br>密钥不在网页中输入或保存。</div><div class="form-actions"><button class="primary">保存配置</button><button type="button" class="secondary" id="ai-test">测试已保存配置</button></div><div id="ai-feedback" class="task-status" role="status"></div></form>`;
    $('#ai-form').onsubmit = event => { event.preventDefault(); busy($('#ai-form .primary'),async()=>{ await api('/ai/config','PUT',{enabled:$('#ai-enabled').checked,base_url:$('#ai-url').value.trim(),model:$('#ai-model').value.trim()}); toast('AI 配置已保存'); }); };
    $('#ai-test').onclick = event => busy(event.currentTarget,async()=>{ const box=$('#ai-feedback'); box.textContent='正在测试连接，最多约 15 秒…'; try { await api('/ai/test','POST',{}); box.textContent='连接成功，可以开始生成例句了。'; } catch(error) { box.textContent=error.message; } });
  } catch(error) { if(token===generation) $('#ai-config').innerHTML=errorBox(error); }
}
async function route() {
  const next = location.hash.replace(/^#\//,'').split('?')[0] || 'lookup';
  if (page===next && app.children.length) return;
  if (!allowDiscard()) { location.hash=`/${page}`; return; }
  page = titles[next] ? next : 'lookup'; generation++; lookupSequence++; noteDraft=noteSaved=''; currentEntry=null;
  const token=generation;
  document.querySelectorAll('[data-page]').forEach(link=>{ link.classList.toggle('active',link.dataset.page===page); if(link.dataset.page===page) link.setAttribute('aria-current','page'); else link.removeAttribute('aria-current'); });
  $('#breadcrumb').textContent=titles[page]; document.title=`${titles[page]} · Lexica`; window.scrollTo({top:0,behavior:'instant'});
  if(page==='lookup') { lookupLayout(); await refreshStatus(); if(token===generation && !ready) $('#dict-banner').innerHTML='<div class="banner">还没有可用词库，或本地服务未连接。请检查服务并导入词库。<a href="#/settings">前往设置 →</a></div>'; }
  if(page==='vocab') vocabPage();
  if(page==='stats') statsPage(token);
  if(page==='review') reviewPage();
  if(page==='settings') settingsPage(token);
}
window.addEventListener('hashchange',route);
window.addEventListener('beforeunload',event=>{ if(noteDraft!==noteSaved){event.preventDefault();event.returnValue='';} });
document.addEventListener('keydown',event=>{ if((event.ctrlKey||event.metaKey)&&event.key==='k'){event.preventDefault();if(page==='lookup')$('#search-input').focus();else location.hash='/lookup';} });
refreshStatus(); route();
