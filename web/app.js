/*
 * app.js - connects this web page to the C engine (filedb.wasm).
 *
 * All database work is done by the C code. This file only:
 *   1. sends each command you type to the C function execute()
 *   2. shows what the C code printed
 *   3. draws the table list, the records grid and the raw file bytes
 */
(function () {
  'use strict';

  var DATA = '/data';          // same folder name as DATA_DIR in filedb.h
  var MAX_LEN = 32;            // same as MAX_LEN in filedb.h (bytes per value)
  var SHOW_BYTES = 4096;       // the file inspector shows at most this many bytes
  var SOURCE_URL = 'https://github.com/dmrk22/filedb';   // link to the source code

  var COMMANDS = ['create', 'insert', 'show', 'find', 'sort', 'count', 'update', 'delete',
                  'tables', 'describe', 'compare', 'convert', 'drop', 'help', 'clear'];

  var M = null;                // the compiled C program (Emscripten module)
  var printed = [];            // lines printed by C during one command
  var current = null;          // name of the selected table
  var fileView = 'readable';   // 'readable' or 'bytes'
  var lastChange = null;       // { path, idx: Set of changed byte positions, text }
  var prevRows = {};           // table name -> rows before the last command
  var pastCommands = [];      // commands typed so far, for the Up/Down keys
  var histPos = 0;
  var busy = false;
  var saved = true;            // false if the browser can't keep data (private mode)
  var cellsByByte = [];        // byte index -> elements that show that byte

  var $ = function (s) { return document.querySelector(s); };
  var el = {
    pill: $('#engine-pill'), status: $('#engine-status'),
    crumbView: $('#crumb-view'), crumbTable: $('#crumb-table'), crumbSep: $('#crumb-sep2'),
    tableList: $('#table-list'), tour: $('#tour'), tourCount: $('#tour-count'),
    tableName: $('#table-name'), tableFormat: $('#table-format'), tableStats: $('#table-stats'),
    toolbar: $('#toolbar'), grid: $('#grid'), gridEmpty: $('#grid-empty'),
    console: $('#console'), form: $('#console-form'), input: $('#console-input'), run: $('#btn-run'),
    filePath: $('#file-path'), fileChange: $('#file-change'), fileBody: $('#file-body'),
    inspect: $('#file-inspect'), legend: $('#legend'), viewToggle: $('#view-toggle')
  };

  /* ================================================================
   *  Small helpers
   * ================================================================ */
  function esc(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  function hex2(n) { return (n < 16 ? '0' : '') + n.toString(16).toUpperCase(); }
  function hex4(n) { return ('0000' + n.toString(16).toUpperCase()).slice(-4); }
  function plural(n, word) { return n.toLocaleString() + '\u00A0' + word + (n === 1 ? '' : 's'); }  /* no line break inside "520 bytes" */
  var decoder = new TextDecoder();

  /* Read a C string (ends at the first 0 byte) from a fixed 32-byte slot. */
  function cString(bytes, start) {
    var end = start;
    while (end < start + MAX_LEN && end < bytes.length && bytes[end] !== 0) end++;
    return decoder.decode(bytes.subarray(start, end));
  }

  /* ================================================================
   *  Reading the table files (only to DRAW them - C does the real work)
   * ================================================================ */
  function readBytes(path) {
    try { return M.FS.readFile(path); } catch (e) { return null; }
  }

  function tableFiles() {
    var names;
    try { names = M.FS.readdir(DATA); } catch (e) { return []; }
    return names.filter(function (n) { return /^[A-Za-z][A-Za-z0-9_]*\.(txt|bin)$/.test(n); }).sort();
  }

  function parseTable(file) {
    var t = {
      name: file.replace(/\.(txt|bin)$/, ''),
      format: /\.txt$/.test(file) ? 'text' : 'binary',
      path: DATA + '/' + file,
      cols: [], rows: [], header: 0, recSize: 0, bytes: null
    };
    t.bytes = readBytes(t.path) || new Uint8Array(0);

    if (t.format === 'text') {
      var lines = decoder.decode(t.bytes).split('\n').filter(function (l) { return l.length > 0; });
      if (lines.length) {
        t.cols = lines[0].split(',');
        t.rows = lines.slice(1).map(function (l) { return l.split(','); });
      }
    } else if (t.bytes.length >= 8) {
      var n = new DataView(t.bytes.buffer, t.bytes.byteOffset, t.bytes.byteLength).getInt32(4, true);
      t.header = 8 + n * MAX_LEN;
      t.recSize = n * MAX_LEN;
      for (var c = 0; c < n; c++) t.cols.push(cString(t.bytes, 8 + c * MAX_LEN));
      for (var off = t.header; t.recSize > 0 && off + t.recSize <= t.bytes.length; off += t.recSize) {
        var row = [];
        for (c = 0; c < n; c++) row.push(cString(t.bytes, off + c * MAX_LEN));
        t.rows.push(row);
      }
    }
    return t;
  }

  function allTables() { return tableFiles().map(parseTable); }
  function findTable(name) {
    var list = allTables();
    for (var i = 0; i < list.length; i++) if (list[i].name === name) return list[i];
    return null;
  }

  /* Copy every file's bytes, so we can see what a command changed. */
  function snapshot() {
    var snap = {};
    tableFiles().forEach(function (f) { snap[DATA + '/' + f] = readBytes(DATA + '/' + f); });
    return snap;
  }

  /* ================================================================
   *  Saving to the browser (IndexedDB), so tables survive a refresh
   * ================================================================ */
  function sync(load) {
    return new Promise(function (resolve) {
      try {
        M.FS.syncfs(load, function (err) { if (err) saved = false; resolve(); });
      } catch (e) { saved = false; resolve(); }
    });
  }

  /* ================================================================
   *  Console
   * ================================================================ */
  function line(text, cls) {
    var d = document.createElement('div');
    d.textContent = text;
    if (cls) d.className = cls;
    el.console.appendChild(d);
  }
  function gap() { line('', 'c-gap'); }
  function info(text) { line(text, 'c-info'); }
  function scrollConsole() { el.console.scrollTop = el.console.scrollHeight; }

  function showOutput(lines) {
    lines.forEach(function (t) {
      var cls = '';
      if (/^Error/.test(t)) cls = 'c-err';
      else if (/^Usage/.test(t)) cls = 'c-info';
      else if (/^OK/.test(t)) cls = 'c-ok';
      else if (/^\+[-+]+\+$/.test(t)) cls = 'c-border';
      line(t, cls);
    });
  }

  /* ================================================================
   *  Running one command through the C engine
   * ================================================================ */
  async function run(cmd) {
    cmd = cmd.trim();
    if (!cmd || !M) return;
    pastCommands.push(cmd);
    histPos = pastCommands.length;

    var words = cmd.split(/\s+/);
    var word = words[0].toLowerCase();
    if (word === 'clear') { el.console.innerHTML = ''; return; }

    line(cmd, 'c-cmd');
    if (word === 'exit' || word === 'quit') {
      info('This is the web version, so there is nothing to exit. Close the tab when you are done.');
      gap(); scrollConsole();
      return;
    }

    var before = snapshot();
    var rowsBefore = {};
    allTables().forEach(function (t) { rowsBefore[t.name] = t.rows; });

    printed = [];
    M.ccall('execute', 'number', ['string'], [cmd]);   // <-- the C code runs here
    showOutput(printed);
    gap(); scrollConsole();

    await sync(false);
    afterCommand(word, words[1], before, rowsBefore);
  }

  /* Decide which table to show and describe what happened to its file. */
  function afterCommand(word, target, before, rowsBefore) {
    var tables = allTables();
    var names = tables.map(function (t) { return t.name; });

    if (target && names.indexOf(target) >= 0) current = target;
    if (current && names.indexOf(current) < 0) current = names.length ? names[0] : null;
    if (!current && names.length) current = names[0];
    setParam('table', current);

    prevRows = rowsBefore;
    var t = current ? findTable(current) : null;
    lastChange = t ? describeChange(word, target, t, before) : null;

    var wasThere = before[DATA + '/' + target + '.txt'] || before[DATA + '/' + target + '.bin'];
    if (word === 'drop' && wasThere && names.indexOf(target) < 0) {
      lastChange = { path: t ? t.path : null, idx: new Set(),
                     text: 'Table <strong>' + esc(target) + '</strong> was dropped and its file was deleted.' };
    }
    renderAll();
  }

  /* Compare the file before and after, byte by byte. */
  function describeChange(word, target, t, before) {
    var old = before[t.path];
    var now = t.bytes;
    var idx = new Set();
    var first = -1;
    var i;

    if (old === undefined || old === null) {
      for (i = 0; i < now.length; i++) idx.add(i);
      var made = word === 'convert' ? 'Converted: a new ' + t.format + ' file was written and the old file was deleted. '
                                    : 'New file created. ';
      return { path: t.path, idx: idx, text: made + '<strong>' + plural(now.length, 'byte') + '</strong> written.' };
    }

    for (i = 0; i < now.length; i++) {
      if (i >= old.length || old[i] !== now[i]) { idx.add(i); if (first < 0) first = i; }
    }
    var count = idx.size;
    var sameTable = target === t.name;

    if (count === 0 && old.length === now.length) {
      return { path: t.path, idx: idx, text: sameTable ? 'This command did not change the file.' : '' };
    }

    var text;
    if (word === 'insert' && now.length > old.length && first === old.length) {
      text = 'Appended <strong>' + plural(now.length - old.length, 'byte') + '</strong> to the end of the file. Nothing before them was touched.';
    } else if (word === 'update' && t.format === 'binary') {
      text = 'Updated in place: <code>fseek()</code> jumped to byte ' + first + ' and overwrote only the matching record. <strong>' +
             plural(count, 'byte') + '</strong> ' + (count === 1 ? 'is' : 'are') + ' different. The rest of the file was not touched.';
    } else if (word === 'update' || word === 'delete') {
      text = 'Rewrote the whole file (' + plural(now.length, 'byte') + ', was ' + old.length + '). <strong>' +
             plural(count, 'byte') + '</strong> ended up different, from byte ' + first + ' on.';
    } else {
      text = '<strong>' + plural(count, 'byte') + '</strong> changed.';
    }
    return { path: t.path, idx: idx, text: text };
  }

  /* ================================================================
   *  Drawing the page
   * ================================================================ */
  function renderAll() {
    var tables = allTables();
    renderTableList(tables);
    var t = null;
    for (var i = 0; i < tables.length; i++) if (tables[i].name === current) t = tables[i];
    renderHeader(t);
    renderGrid(t);
    renderFile(t);
  }

  function renderTableList(tables) {
    if (!tables.length) {
      el.tableList.innerHTML = '<li class="table-none">No tables yet.</li>';
      return;
    }
    el.tableList.innerHTML = tables.map(function (t) {
      return '<li><button type="button" class="table-item' + (t.name === current ? ' is-active' : '') +
        '" data-table="' + esc(t.name) + '">' +
        '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3.5" y="4.5" width="17" height="15" rx="2" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M3.5 9.5h17M9.5 9.5v10" stroke="currentColor" stroke-width="1.6"/></svg>' +
        '<span class="table-item-name">' + esc(t.name) + '</span>' +
        '<span class="table-item-meta">' + (t.format === 'text' ? 'txt' : 'bin') + ' ' + t.rows.length + '</span>' +
        '</button></li>';
    }).join('');
  }

  function renderHeader(t) {
    var buttons = el.toolbar.querySelectorAll('button');
    for (var i = 0; i < buttons.length; i++) buttons[i].disabled = !t;
    el.crumbTable.textContent = t ? t.name : 'no table';

    if (!t) {
      el.tableName.textContent = 'Select a table';
      el.tableFormat.hidden = true;
      el.tableStats.textContent = '';
      return;
    }
    el.tableName.textContent = t.name;
    el.tableFormat.hidden = false;
    el.tableFormat.textContent = t.format === 'text' ? 'text file' : 'binary file';
    el.tableFormat.className = 'badge ' + (t.format === 'text' ? 'is-text' : 'is-binary');
    el.tableStats.textContent = plural(t.rows.length, 'record') + ', ' + plural(t.cols.length, 'column') +
      ', ' + plural(t.bytes.length, 'byte');
    el.toolbar.querySelector('[data-action="convert"]').textContent =
      'Convert to ' + (t.format === 'text' ? 'binary' : 'text');
  }

  function renderGrid(t) {
    if (!t) {
      el.grid.hidden = true;
      el.gridEmpty.hidden = false;
      return;
    }
    el.grid.hidden = false;
    el.gridEmpty.hidden = true;

    var old = prevRows[t.name] || null;
    var html = '<thead><tr><th class="rownum" title="Record number, counted from 0 like in C">#</th>';
    t.cols.forEach(function (c) { html += '<th>' + esc(c) + '<span class="col-type">char[32]</span></th>'; });
    html += '</tr></thead><tbody>';

    if (!t.rows.length) {
      html += '<tr><td class="rownum"></td><td colspan="' + t.cols.length + '" style="color:var(--faint)">No records yet. Click "Insert row" to add one.</td></tr>';
    }
    t.rows.forEach(function (r, i) {
      var isNew = old && (i >= old.length || JSON.stringify(old[i]) !== JSON.stringify(r));
      html += '<tr' + (isNew ? ' class="is-new"' : '') + '><td class="rownum">' + i + '</td>';
      t.cols.forEach(function (c, k) { html += '<td>' + esc(r[k] === undefined ? '' : r[k]) + '</td>'; });
      html += '</tr>';
    });
    el.grid.innerHTML = html + '</tbody>';
    prevRows[t.name] = null;
  }

  /* ---------- File inspector ---------- */

  /* Which part of the file is byte i? Returns a CSS class. */
  function regionOf(t, i) {
    var b = t.bytes[i];
    if (t.format === 'binary') {
      if (i < 4) return 'r-magic';
      if (i < 8) return 'r-count';
      if (i < t.header) return b === 0 ? 'r-zero' : 'r-cols';
      var rec = Math.floor((i - t.header) / t.recSize) % 2 === 0 ? 'r-rec-a' : 'r-rec-b';
      return b === 0 ? rec + ' r-zero' : rec;     /* zeros keep their record's band */
    }
    if (b === 44) return 't-sep';
    if (b === 10) return 't-nl';
    var lineNo = lineOf(t, i);
    if (lineNo === 0) return 'r-cols';
    return lineNo % 2 === 1 ? 'r-rec-a' : 'r-rec-b';
  }

  var lineStarts = [];
  function computeLines(t) {
    lineStarts = [0];
    for (var i = 0; i < t.bytes.length; i++) if (t.bytes[i] === 10) lineStarts.push(i + 1);
  }
  function lineOf(t, i) {
    var lo = 0, hi = lineStarts.length - 1;
    while (lo < hi) { var mid = (lo + hi + 1) >> 1; if (lineStarts[mid] <= i) lo = mid; else hi = mid - 1; }
    return lo;
  }

  function renderFile(t) {
    cellsByByte = [];
    cache = { name: t ? t.name : null, table: t };   // remember the table for hovering
    hovered = -1;
    if (!t) {
      el.filePath.textContent = '—';
      el.fileChange.innerHTML = lastChange && lastChange.text ? lastChange.text : '';
      el.fileBody.innerHTML = '<p class="muted-note">The selected table\'s file appears here.</p>';
      el.legend.innerHTML = '';
      return;
    }
    if (t.format === 'text') computeLines(t);

    var isBinary = t.format === 'binary';
    var view = isBinary ? 'bytes' : fileView;
    var readableBtn = el.viewToggle.querySelector('[data-fview="readable"]');
    var bytesBtn = el.viewToggle.querySelector('[data-fview="bytes"]');
    readableBtn.disabled = isBinary;
    readableBtn.title = isBinary ? 'Binary files are not readable text' : '';
    readableBtn.setAttribute('aria-pressed', view === 'readable');
    bytesBtn.setAttribute('aria-pressed', view === 'bytes');

    el.filePath.textContent = t.path.slice(1) + ' (' + plural(t.bytes.length, 'byte') + ')';
    var change = lastChange && lastChange.path === t.path ? lastChange : null;
    el.fileChange.innerHTML = change && change.text ? change.text
      : (isBinary ? 'Binary: a header, then fixed-size records of ' + t.recSize + ' bytes each.'
                  : 'Text: one line per record, values separated by commas.');

    var changed = change ? change.idx : new Set();
    var n = Math.min(t.bytes.length, SHOW_BYTES);
    var html = view === 'bytes' ? bytesHtml(t, n, changed) : textHtml(t, n, changed);
    if (t.bytes.length > n) {
      html += '<p class="muted-note">Showing the first ' + SHOW_BYTES.toLocaleString() + ' of ' + plural(t.bytes.length, 'byte') + '.</p>';
    }
    el.fileBody.innerHTML = html;

    var nodes = el.fileBody.querySelectorAll('[data-i]');
    for (var i = 0; i < nodes.length; i++) {
      var k = +nodes[i].getAttribute('data-i');
      (cellsByByte[k] = cellsByByte[k] || []).push(nodes[i]);
    }
    el.inspect.textContent = 'Point at a byte to see what it stores.';
    renderLegend(t, view);

    /* Scroll so the first changed byte is in view. */
    var first = -1;
    changed.forEach(function (k) { if (first < 0 || k < first) first = k; });
    if (first >= 0 && cellsByByte[first]) {
      var node = cellsByByte[first][0];
      el.fileBody.scrollTop = Math.max(0, node.offsetTop - el.fileBody.clientHeight / 3);
    }
  }

  /* Readable view: one block per line, every other record line banded like green-bar paper. */
  function textHtml(t, n, changed) {
    var html = '<pre class="text-view">';
    var open = false;
    for (var i = 0; i < n; i++) {
      var b = t.bytes[i];
      var lineNo = lineOf(t, i);
      if (!open) {
        html += '<span class="tl' + (lineNo > 0 && lineNo % 2 === 0 ? ' tl-band' : '') + '">';
        open = true;
      }
      var cls = regionOf(t, i);
      if (cls === 'r-rec-a' || cls === 'r-rec-b' || cls === 'r-cols') cls = '';
      if (lineNo === 0 && b !== 10) cls = 't-head';
      if (changed.has(i)) cls += ' chg';
      if (b === 10) {
        html += '<i data-i="' + i + '" class="' + cls + '">↵</i></span>';
        open = false;
        continue;
      }
      var ch = b >= 32 && b < 127 ? esc(String.fromCharCode(b)) : '·';
      html += '<i data-i="' + i + '" class="' + cls + '">' + ch + '</i>';
    }
    if (open) html += '</span>';
    return html + '</pre>';
  }

  function bytesHtml(t, n, changed) {
    var html = '<div class="hex">';
    for (var row = 0; row < n; row += 16) {
      var hexPart = '', asciiPart = '';
      for (var k = 0; k < 16; k++) {
        var i = row + k;
        if (i >= n) { hexPart += '<b></b>'; continue; }
        var b = t.bytes[i];
        var cls = regionOf(t, i) + (changed.has(i) ? ' chg' : '');
        hexPart += '<b data-i="' + i + '" class="' + cls + '">' + hex2(b) + '</b>';
        var ch = b >= 32 && b < 127 ? esc(String.fromCharCode(b)) : '.';
        asciiPart += '<i data-i="' + i + '" class="' + cls + '">' + ch + '</i>';
      }
      html += '<div class="hex-row"><span class="hex-off">' + hex4(row) + '</span>' + hexPart +
              '<span></span><span class="hex-ascii">' + asciiPart + '</span></div>';
    }
    return html + '</div>';
  }

  function renderLegend(t, view) {
    var items = t.format === 'binary'
      ? [['var(--ochre)', 'File header: “FDB1” + column count'], ['var(--blue)', 'Column names'],
         ['var(--band)', 'Records (every other one banded)'], ['var(--faint)', 'Unused space (00)'],
         ['var(--hl)', 'Changed by the last command']]
      : view === 'bytes'
        ? [['var(--blue)', 'Header line'], ['var(--band)', 'Records (every other one banded)'], ['var(--ochre)', 'Comma (2C)'],
           ['var(--faint)', 'Line end (0A)'], ['var(--hl)', 'Changed by the last command']]
        : [['var(--blue)', 'Header line (column names)'], ['var(--ochre)', 'Comma'], ['var(--faint)', 'Line end ↵'],
           ['var(--band)', 'Every other record banded'], ['var(--hl)', 'Changed by the last command']];
    el.legend.innerHTML = items.map(function (it) {
      return '<li><i style="background:' + it[0] + '"></i>' + esc(it[1]) + '</li>';
    }).join('');
  }

  /* Explain one byte in plain words. */
  function explainByte(t, i) {
    var b = t.bytes[i];
    var head = 'Byte ' + i + ' = 0x' + hex2(b) + ': ';
    if (t.format === 'binary') {
      if (i < 4) return head + 'part of the signature “FDB1” that marks a FileDB binary file.';
      if (i < 8) return head + 'part of the column count, a 4-byte int = ' + t.cols.length + '.';
      if (i < t.header) {
        var c = Math.floor((i - 8) / MAX_LEN);
        return head + 'header, the 32-byte name slot of column ' + c + ' (“' + t.cols[c] + '”)' + (b === 0 ? ', unused space.' : '.');
      }
      var r = Math.floor((i - t.header) / t.recSize);
      var col = Math.floor(((i - t.header) % t.recSize) / MAX_LEN);
      var v = t.rows[r] ? t.rows[r][col] : '';
      return head + 'record ' + r + ', column “' + t.cols[col] + '” = “' + v + '”' + (b === 0 ? ', unused space in its 32-byte slot.' : '.');
    }
    var L = lineOf(t, i);
    var what = L === 0 ? 'line 1, the header line with column names' : 'line ' + (L + 1) + ', record ' + (L - 1);
    if (b === 44) return head + what + ': a comma that separates two values.';
    if (b === 10) return head + what + ': the end of the line (\\n).';
    return head + what + ': the character “' + String.fromCharCode(b) + '”.';
  }

  function highlight(t, i, on) {
    var from = i, to = i;
    if (t.format === 'binary' && i >= 8) {           // the whole 32-byte slot
      from = i - ((i - 8) % MAX_LEN);
      to = from + MAX_LEN - 1;
    }
    for (var k = from; k <= to; k++) {
      (cellsByByte[k] || []).forEach(function (n) { n.classList.toggle('is-hl', on); });
    }
  }

  var hovered = -1;
  el.fileBody.addEventListener('mouseover', function (e) {
    var node = e.target.closest('[data-i]');
    var t = current ? findTableCached() : null;
    if (!node || !t) return;
    var i = +node.getAttribute('data-i');
    if (i === hovered) return;
    if (hovered >= 0) highlight(t, hovered, false);
    hovered = i;
    highlight(t, i, true);
    el.inspect.textContent = explainByte(t, i);
  });
  el.fileBody.addEventListener('mouseleave', function () {
    var t = current ? findTableCached() : null;
    if (t && hovered >= 0) highlight(t, hovered, false);
    hovered = -1;
  });

  var cache = { name: null, table: null };
  function findTableCached() {
    if (cache.name !== current || !cache.table) { cache.name = current; cache.table = findTable(current); }
    return cache.table;
  }

  /* ================================================================
   *  Sample data, quick-start tour and command reference
   * ================================================================ */
  var SAMPLE = [
    'create students text id name dept marks',
    'insert students 1 Ravi CSE 85',
    'insert students 2 "Anita Sharma" ECE 92',
    'insert students 3 Kiran CSE 78',
    'insert students 4 Meena MECH 58',
    'insert students 5 Arjun EEE 100',
    'create books binary id title author price',
    'insert books 1 "The C Programming Language" Kernighan 450',
    'insert books 2 "Let Us C" Kanetkar 300',
    'insert books 3 "Data Structures in C" Tanenbaum 520'
  ];

  var TOUR = [
    ['Make a text table', ['create students text id name dept marks']],
    ['Add a record', ['insert students 1 Ravi CSE 85']],
    ['Add four more', ['insert students 2 "Anita Sharma" ECE 92', 'insert students 3 Kiran CSE 78',
                       'insert students 4 Meena MECH 58', 'insert students 5 Arjun EEE 100']],
    ['Show all records', ['show students']],
    ['Search: marks above 80', ['find students marks > 80']],
    ['Sort by marks (as numbers)', ['sort students marks desc']],
    ['Count CSE students', ['count students dept = CSE']],
    ['Update a text file', ['update students set marks 95 where id = 3']],
    ['Make a binary table', ['create books binary id title author price', 'insert books 1 "Let Us C" Kanetkar 300',
                             'insert books 2 "Data Structures in C" Tanenbaum 520']],
    ['Update in place with fseek', ['update books set price 350 where id = 1']],
    ['Compare file sizes', ['compare students']],
    ['Delete a record', ['delete students where dept = MECH']]
  ];

  var CMD_DOCS = [
    ['create <table> <text|binary> <col1> <col2> …', 'Make a new table and choose how it is stored.', 'create pets text id name age'],
    ['insert <table> <value1> <value2> …', 'Add one record. Give one value for each column.', 'insert students 6 Divya CSE 88'],
    ['show <table>', 'Show every record.', 'show students'],
    ['find <table> <col> <op> <value>', 'Show only the records that match a condition.', 'find students dept = CSE'],
    ['sort <table> <col> [asc|desc]', 'Show the records in order. The file is not changed.', 'sort students marks desc'],
    ['count <table> [<col> <op> <value>]', 'Count all records, or only the matching ones.', 'count students marks >= 80'],
    ['update <table> set <col> <value> where <col> <op> <value>', 'Change one value in every matching record.', 'update students set dept IT where id = 1'],
    ['delete <table> where <col> <op> <value>', 'Remove every matching record.', 'delete students where marks < 60'],
    ['tables', 'List all tables with their format, records and file size.', 'tables'],
    ['describe <table>', 'Show the columns, format, file size and, for binary, the byte layout.', 'describe books'],
    ['compare <table>', 'Save the same records both ways and compare the file sizes.', 'compare students'],
    ['convert <table> <text|binary>', 'Change how a table is stored.', 'convert students binary'],
    ['drop <table>', 'Delete a table and its file.', 'drop pets'],
    ['help', 'List all commands.', 'help']
  ];

  function tourDone() {
    try { return JSON.parse(localStorage.getItem('filedb-tour') || '[]'); } catch (e) { return []; }
  }
  function setTourDone(list) {
    try { localStorage.setItem('filedb-tour', JSON.stringify(list)); } catch (e) { /* ignore */ }
  }
  function renderTour() {
    var done = tourDone();
    el.tour.innerHTML = TOUR.map(function (s, i) {
      return '<li><button type="button" class="tour-btn' + (done.indexOf(i) >= 0 ? ' is-done' : '') + '" data-step="' + i + '">' +
        '<span class="tour-num">' + (done.indexOf(i) >= 0 ? '✓' : i + 1) + '</span>' +
        '<span class="tour-text">' + esc(s[0]) + '<span class="tour-cmd">' + esc(s[1][0]) + '</span></span></button></li>';
    }).join('');
    el.tourCount.textContent = done.length + '/' + TOUR.length;
  }

  async function runMany(cmds) {
    if (busy) return;
    busy = true;
    setInputEnabled(false);
    for (var i = 0; i < cmds.length; i++) await run(cmds[i]);
    busy = false;
    setInputEnabled(true);
  }

  function renderCommandDocs() {
    $('#cmd-rows').innerHTML = CMD_DOCS.map(function (c) {
      return '<tr><td>' + esc(c[0]) + '</td><td>' + esc(c[1]) + '</td><td><button type="button" class="run-example" data-run="' +
        esc(c[2]) + '">' + esc(c[2]) + '</button></td></tr>';
    }).join('');
  }

  /* ================================================================
   *  Views (table editor / documentation)
   * ================================================================ */
  function showView(name) {
    document.querySelectorAll('.view').forEach(function (v) { v.classList.toggle('is-active', v.id === 'view-' + name); });
    document.querySelectorAll('.rail-btn[data-view]').forEach(function (b) { b.classList.toggle('is-active', b.getAttribute('data-view') === name); });
    el.crumbView.textContent = name === 'docs' ? 'Documentation' : 'Table editor';
    el.crumbTable.hidden = el.crumbSep.hidden = name === 'docs';
    $('.topbar [data-goto="docs"]').hidden = name === 'docs';
    setParam('view', name === 'docs' ? 'docs' : null);
    if (name === 'studio') focusInput();
  }

  /* Keep the view and the selected table in the address, so a link opens the same screen. */
  function setParam(key, value) {
    try {
      var url = new URL(location.href);
      if (value) url.searchParams.set(key, value); else url.searchParams.delete(key);
      window.history.replaceState(null, '', url);
    } catch (e) { /* ignore */ }
  }

  /* Focus the console on desktop only: on phones it would pop up the keyboard. */
  function focusInput() {
    if (window.matchMedia && matchMedia('(pointer: fine)').matches) el.input.focus();
  }

  /* ================================================================
   *  Events
   * ================================================================ */
  function setInputEnabled(on) {
    el.input.disabled = !on;
    el.run.disabled = !on;
    if (on) focusInput();
  }

  el.form.addEventListener('submit', function (e) {
    e.preventDefault();
    var cmd = el.input.value;
    el.input.value = '';
    runMany([cmd]);
  });

  el.input.addEventListener('keydown', function (e) {
    if (e.key === 'ArrowUp' && histPos > 0) {
      histPos--; el.input.value = pastCommands[histPos]; e.preventDefault();
    } else if (e.key === 'ArrowDown') {
      if (histPos < pastCommands.length - 1) { histPos++; el.input.value = pastCommands[histPos]; }
      else { histPos = pastCommands.length; el.input.value = ''; }
      e.preventDefault();
    } else if (e.key === 'Tab') {
      e.preventDefault();
      complete();
    }
  });

  /* Tab completion: the command word first, then a table name. */
  function complete() {
    var value = el.input.value;
    var parts = value.split(' ');
    var last = parts[parts.length - 1];
    var options = parts.length === 1 ? COMMANDS : allTables().map(function (t) { return t.name; });
    if (parts.length > 2) return;
    var hits = options.filter(function (o) { return o.indexOf(last) === 0; });
    if (!hits.length) return;
    var common = hits.reduce(function (a, b) { var i = 0; while (i < a.length && a[i] === b[i]) i++; return a.slice(0, i); });
    parts[parts.length - 1] = hits.length === 1 ? hits[0] + ' ' : common;
    el.input.value = parts.join(' ');
  }

  document.addEventListener('click', function (e) {
    var b;
    if ((b = e.target.closest('[data-table]'))) {
      current = b.getAttribute('data-table'); lastChange = null; setParam('table', current); renderAll(); return;
    }
    if ((b = e.target.closest('[data-step]'))) {
      var i = +b.getAttribute('data-step');
      runMany(TOUR[i][1]).then(function () {
        var done = tourDone(); if (done.indexOf(i) < 0) done.push(i); setTourDone(done); renderTour();
      });
      return;
    }
    if ((b = e.target.closest('[data-run]'))) { showView('studio'); runMany([b.getAttribute('data-run')]); return; }
    if ((b = e.target.closest('[data-view]'))) { showView(b.getAttribute('data-view')); return; }
    if ((b = e.target.closest('[data-goto]'))) { showView(b.getAttribute('data-goto')); return; }
    if ((b = e.target.closest('[data-fview]')) && !b.disabled) {
      fileView = b.getAttribute('data-fview'); renderAll(); return;
    }
    if ((b = e.target.closest('[data-action]'))) { toolbarAction(b.getAttribute('data-action')); }
  });

  function prefill(cmd, hint) {
    el.input.value = cmd;
    el.input.focus();
    el.input.setSelectionRange(cmd.length, cmd.length);
    if (hint) { info(hint); gap(); scrollConsole(); }
  }

  function toolbarAction(action) {
    var t = current ? findTable(current) : null;
    if (!t) return;
    if (action === 'insert') prefill('insert ' + t.name + ' ', 'Type one value for each column: ' + t.cols.join(' ') + ' (use "quotes" for spaces), then press Enter.');
    if (action === 'find') prefill('find ' + t.name + ' ' + t.cols[0] + ' = ', 'Finish the condition, for example: find ' + t.name + ' ' + t.cols[0] + ' = 1');
    if (action === 'compare') runMany(['compare ' + t.name]);
    if (action === 'convert') runMany(['convert ' + t.name + ' ' + (t.format === 'text' ? 'binary' : 'text')]);
    if (action === 'drop') prefill('drop ' + t.name, 'Press Enter to delete the table “' + t.name + '” and its file.');
  }

  $('#btn-clear').addEventListener('click', function () { el.console.innerHTML = ''; el.input.focus(); });

  $('#btn-sample').addEventListener('click', function () {
    var cmds = [];
    ['students', 'books'].forEach(function (n) { if (findTable(n)) cmds.push('drop ' + n); });
    runMany(cmds.concat(SAMPLE)).then(function () { current = 'students'; lastChange = null; renderAll(); });
  });

  $('#btn-reset').addEventListener('click', function () {
    var names = allTables().map(function (t) { return t.name; });
    if (!names.length) return;
    if (!window.confirm('Delete all ' + plural(names.length, 'table') + ' and their files? This cannot be undone.')) return;
    setTourDone([]);
    renderTour();
    runMany(names.map(function (n) { return 'drop ' + n; }));
  });

  /* New-table dialog */
  var dialog = $('#new-table-dialog');
  var dform = $('#new-table-form');
  function dialogCommand() {
    var f = dform.elements;
    var name = f.name.value.trim() || 'students';
    var cols = f.cols.value.trim().split(/\s+/).filter(Boolean).join(' ') || 'id name dept marks';
    return 'create ' + name + ' ' + f.format.value + ' ' + cols;
  }
  $('#btn-new-table').addEventListener('click', function () {
    dform.reset();
    $('#new-table-preview').textContent = dialogCommand();
    dialog.showModal();
  });
  dform.addEventListener('input', function () { $('#new-table-preview').textContent = dialogCommand(); });
  dform.addEventListener('submit', function (e) {
    e.preventDefault();
    var action = e.submitter ? e.submitter.value : 'create';
    dialog.close();
    if (action === 'create') runMany([dialogCommand()]);
  });

  /* ================================================================
   *  Start-up
   * ================================================================ */
  function setStatus(state, text) {
    el.pill.classList.remove('is-ready', 'is-error');
    if (state) el.pill.classList.add('is-' + state);
    el.status.textContent = text;
  }

  async function boot() {
    renderTour();
    renderCommandDocs();
    if (SOURCE_URL) {
      ['#rail-source', '#docs-source'].forEach(function (s) { $(s).href = SOURCE_URL; });
    } else {
      $('#rail-source').hidden = true;
      $('#docs-source').hidden = true;
    }

    try {
      M = await createFileDB({
        print: function (t) { printed.push(t); },
        printErr: function (t) { printed.push(t); }
      });
      M.FS.mkdir(DATA);
      try { M.FS.mount(M.IDBFS, {}, DATA); } catch (e) { saved = false; }
      await sync(true);
    } catch (e) {
      setStatus('error', 'C engine failed to load');
      info('The C engine could not start: ' + e + '. Try a recent Chrome, Edge, Firefox or Safari.');
      return;
    }

    setStatus('ready', 'C engine ready');
    setInputEnabled(true);
    var tables = allTables();
    var wanted = new URLSearchParams(location.search).get('table');     /* ?table=books opens that table */
    if (tables.length) current = tables.some(function (t) { return t.name === wanted; }) ? wanted : tables[0].name;

    info('FileDB is running. Each command you type here is passed to the C function execute(), compiled to WebAssembly.');
    info(tables.length ? 'Your tables from last time were loaded from this browser\'s storage.'
                       : 'Type help to see every command, or click “Load sample data” on the left.');
    if (!saved) info('Note: this browser is not allowing storage, so tables will be lost when you close the tab.');
    gap();
    renderAll();
    await demo();
  }

  /*
   * Demo links for presenting:  ?demo=text  or  ?demo=binary   (add &view=docs for the docs page)
   * They load the sample data and make one change, so the highlight is visible.
   */
  async function demo() {
    var params = new URLSearchParams(location.search);
    var mode = params.get('demo');
    if (params.get('view') === 'docs') showView('docs');
    if (mode !== 'text' && mode !== 'binary') return;

    document.body.classList.add('demo');
    var cmds = [];
    allTables().forEach(function (t) { cmds.push('drop ' + t.name); });
    await runMany(cmds.concat(SAMPLE));
    if (mode === 'binary') {
      fileView = 'bytes';
      await runMany(['show books', 'update books set price 350 where id = 2']);
    } else {
      await runMany(['find students marks > 80', 'update students set marks 95 where id = 3']);
    }
  }

  boot();
})();
