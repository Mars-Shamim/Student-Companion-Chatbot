/**
 * ============================================================
 *  Student AI — Master Database Script  v4
 *
 *  Architecture:
 *  ┌─────────────────────────────────────────────────────┐
 *  │  Google Drive                                       │
 *  │  ├── Physics Database    (Spreadsheet)              │
 *  │  │   ├── Physics 1       (Sheet tab)                │
 *  │  │   ├── Physics 2       (Sheet tab)                │
 *  │  ├── Chemistry Database                             │
 *  │  │   ├── Chemistry 1                                │
 *  │  ├── Biology Database                               │
 *  │  ├── Math Database                                  │
 *  │  ├── English Database                               │
 *  │  └── General Database                               │
 *  └─────────────────────────────────────────────────────┘
 *
 *  প্রতিটা Sheet এ ৩ column:
 *  ┌─────────────────┬────────────────────────┬──────────┐
 *  │    Index        │     Explanation        │   Date   │
 *  └─────────────────┴────────────────────────┴──────────┘
 *
 *  এই script টা Master Spreadsheet এ deploy করো।
 *  Master Spreadsheet সব Database Spreadsheet এর ID track করে।
 *
 *  Cell splitting: >49000 char হলে auto split → Index[2], Index[3]
 *  Recall: সব part জোড়া লেগে পুরোটা আসে।
 *
 *  Actions:
 *    remember       — subject+chapter তে save
 *    recall         — search across subject বা chapter
 *    list_subjects  — সব database ও chapter list
 *    delete         — entry delete
 * ============================================================
 */

// ── Config ────────────────────────────────────────────────────
const COLS     = { INDEX: 1, EXPLANATION: 2, DATE: 3 };
const CELL_MAX = 49000;
const MAX_RES  = 5;

// Default subject databases (auto-created in Drive)
const DEFAULT_SUBJECTS = [
  "Physics", "Chemistry", "Biology", "Math", "English", "General"
];

// Master sheet এ DB IDs রাখার sheet name
const MASTER_SHEET = "DB_Registry";

// ── Entry points ──────────────────────────────────────────────
function doPost(e) {
  try {
    const body = JSON.parse(e.postData.contents);
    let result;
    switch (body.action) {
      case "remember":      result = remember(body);      break;
      case "recall":        result = recall(body);        break;
      case "list_subjects": result = listSubjects();      break;
      case "delete":        result = deleteEntry(body);   break;
      default: result = { ok: false, error: "Unknown: " + body.action };
    }
    return json(result);
  } catch (err) {
    return json({ ok: false, error: err.message });
  }
}

function doGet() {
  const dbs = listSubjects();
  return json({ ok: true, status: "running", data: dbs });
}

// ── remember ──────────────────────────────────────────────────
// body: { subject, chapter, index, explanation }
// subject  = "Physics"  → "Physics Database" spreadsheet
// chapter  = "Physics 1" → sheet tab name inside that spreadsheet
// index    = keyword
// explanation = full text
function remember(body) {
  const subject     = (body.subject     || "General").trim();
  const chapter     = (body.chapter     || subject + " 1").trim();
  const indexText   = (body.index       || "").trim();
  const explanation = (body.explanation || "").trim();

  if (!indexText)   return { ok: false, error: "index required" };
  if (!explanation) return { ok: false, error: "explanation required" };

  const sheet = getChapterSheet(subject, chapter);

  // আগের entry থাকলে মুছে নতুন লেখো
  deleteAllParts(sheet, indexText);

  // Cell limit এ ভাগ করো
  const parts = splitIntoParts(explanation);
  const now   = getTimestamp();

  for (let i = 0; i < parts.length; i++) {
    const rowIdx = parts.length === 1 ? indexText
                 : i === 0            ? indexText
                 : `${indexText}[${i + 1}]`;
    sheet.appendRow([rowIdx, parts[i], now]);
    styleLastRow(sheet, i > 0);  // continuation rows হালকা রঙ
  }

  return {
    ok:      true,
    action:  "saved",
    subject,
    chapter,
    index:   indexText,
    parts:   parts.length,
    chars:   explanation.length,
    date:    now
  };
}

// ── recall ────────────────────────────────────────────────────
// body: { query, subject?, chapter? }
// subject ছাড়া → সব database এ search
// chapter দিলে → শুধু ওই chapter এ search
function recall(body) {
  const query   = (body.query   || "").toLowerCase().trim();
  const subject = (body.subject || "").trim();
  const chapter = (body.chapter || "").trim();
  if (!query) return { ok: false, error: "query required" };

  const registry = getRegistry();
  const queryWords = query.split(/\s+/).filter(w => w.length > 1);
  const found = [];

  // কোন কোন database search করব
  const subjectsToSearch = subject
    ? registry.filter(r => r.subject.toLowerCase() === subject.toLowerCase())
    : registry;

  for (const db of subjectsToSearch) {
    let ss;
    try { ss = SpreadsheetApp.openById(db.id); }
    catch (e) { continue; }

    // কোন কোন sheet (chapter) search করব
    const sheets = chapter
      ? [ss.getSheetByName(chapter)].filter(Boolean)
      : ss.getSheets();

    for (const sheet of sheets) {
      const lastRow = sheet.getLastRow();
      if (lastRow < 2) continue;

      const indexVals = sheet
        .getRange(2, COLS.INDEX, lastRow - 1, 1)
        .getValues();

      for (let i = 0; i < indexVals.length; i++) {
        const raw = String(indexVals[i][0]);
        if (!raw || /\[\d+\]$/.test(raw)) continue;  // continuation skip

        const idx = raw.toLowerCase();
        let score = 0;
        for (const w of queryWords) if (idx.includes(w)) score += 2;
        if (idx.includes(query)) score += 5;

        if (score > 0) {
          found.push({
            score,
            subject:  db.subject,
            chapter:  sheet.getName(),
            baseIdx:  raw,
            sheet
          });
        }
      }
    }
  }

  found.sort((a, b) => b.score - a.score);

  const results = found.slice(0, MAX_RES).map(f => ({
    subject:     f.subject,
    chapter:     f.chapter,
    index:       f.baseIdx,
    explanation: getAllParts(f.sheet, f.baseIdx),
    date:        getEntryDate(f.sheet, f.baseIdx)
  }));

  return { ok: true, results, count: results.length, query };
}

// ── listSubjects ──────────────────────────────────────────────
function listSubjects() {
  const registry = getRegistry();
  const result = [];

  for (const db of registry) {
    let ss;
    try { ss = SpreadsheetApp.openById(db.id); }
    catch (e) {
      result.push({ subject: db.subject, error: "Cannot open" });
      continue;
    }

    const chapters = ss.getSheets().map(s => ({
      name:    s.getName(),
      entries: countBaseEntries(s)
    }));

    result.push({
      subject:  db.subject,
      chapters,
      total:    chapters.reduce((sum, c) => sum + c.entries, 0)
    });
  }

  return { ok: true, databases: result };
}

// ── delete ────────────────────────────────────────────────────
function deleteEntry(body) {
  const subject   = (body.subject || "").trim();
  const chapter   = (body.chapter || "").trim();
  const indexText = (body.index   || "").trim();

  if (!subject || !chapter || !indexText)
    return { ok: false, error: "subject, chapter and index required" };

  const sheet   = getChapterSheet(subject, chapter);
  const deleted = deleteAllParts(sheet, indexText);

  return deleted
    ? { ok: true, message: `Deleted "${indexText}" (${deleted} rows) from ${subject} / ${chapter}` }
    : { ok: false, error: "Not found: " + indexText };
}

// ── Registry (Master sheet) ───────────────────────────────────
// Master Spreadsheet এর "DB_Registry" sheet এ সব DB এর ID রাখে
// Columns: Subject | SpreadsheetID

function getRegistry() {
  const master = SpreadsheetApp.getActiveSpreadsheet();
  let reg = master.getSheetByName(MASTER_SHEET);

  if (!reg) {
    reg = master.insertSheet(MASTER_SHEET);
    reg.getRange(1, 1, 1, 2).setValues([["Subject", "SpreadsheetID"]]);
    const hdr = reg.getRange(1, 1, 1, 2);
    hdr.setBackground("#1a73e8"); hdr.setFontColor("#fff"); hdr.setFontWeight("bold");
    reg.setColumnWidth(1, 150); reg.setColumnWidth(2, 400);
    reg.setFrozenRows(1);
  }

  const lastRow = reg.getLastRow();
  if (lastRow < 2) {
    // Default subjects তৈরি করো
    return ensureDefaultSubjects(reg);
  }

  const vals = reg.getRange(2, 1, lastRow - 1, 2).getValues();
  return vals
    .filter(r => r[0] && r[1])
    .map(r => ({ subject: String(r[0]).trim(), id: String(r[1]).trim() }));
}

function ensureDefaultSubjects(reg) {
  const folder = getDriveFolder();
  const created = [];

  for (const subj of DEFAULT_SUBJECTS) {
    const dbName  = subj + " Database";
    const ss      = createOrFindSpreadsheet(dbName, folder);
    const firstSh = ss.getSheets()[0];

    // First sheet টার নাম set করো
    const firstChapter = subj + " 1";
    if (firstSh.getName() !== firstChapter) {
      firstSh.setName(firstChapter);
    }
    setupSheetHeaders(firstSh);

    reg.appendRow([subj, ss.getId()]);
    created.push({ subject: subj, id: ss.getId() });
  }

  return created;
}

function getDriveFolder() {
  // Script এর parent folder
  const fileId   = ScriptApp.getScriptId();
  const file     = DriveApp.getFileById(fileId);
  const parents  = file.getParents();
  return parents.hasNext() ? parents.next() : DriveApp.getRootFolder();
}

function createOrFindSpreadsheet(name, folder) {
  // একই নামের file আছে কিনা দেখো
  const files = folder.getFilesByName(name);
  while (files.hasNext()) {
    const f = files.next();
    if (f.getMimeType() === MimeType.GOOGLE_SHEETS) {
      return SpreadsheetApp.openById(f.getId());
    }
  }
  // না থাকলে নতুন তৈরি করো
  const ss = SpreadsheetApp.create(name);
  // Folder এ move করো
  DriveApp.getFileById(ss.getId()).moveTo(folder);
  return ss;
}

// ── Chapter sheet ─────────────────────────────────────────────
function getChapterSheet(subject, chapter) {
  const registry = getRegistry();
  const db = registry.find(r => r.subject.toLowerCase() === subject.toLowerCase());

  let ss;
  if (db) {
    ss = SpreadsheetApp.openById(db.id);
  } else {
    // Subject না থাকলে নতুন database তৈরি করো
    const folder = getDriveFolder();
    ss = createOrFindSpreadsheet(subject + " Database", folder);
    const master = SpreadsheetApp.getActiveSpreadsheet();
    const reg    = master.getSheetByName(MASTER_SHEET);
    reg.appendRow([subject, ss.getId()]);
  }

  // Chapter sheet খোঁজো বা তৈরি করো
  let sheet = ss.getSheetByName(chapter);
  if (!sheet) {
    sheet = ss.insertSheet(chapter);
    setupSheetHeaders(sheet);
  } else if (sheet.getLastRow() === 0) {
    setupSheetHeaders(sheet);
  }
  return sheet;
}

// ── Cell splitting ────────────────────────────────────────────
function splitIntoParts(text) {
  if (text.length <= CELL_MAX) return [text];
  const parts = [];
  let rem = text;
  while (rem.length > CELL_MAX) {
    let cut = CELL_MAX;
    const sp = rem.lastIndexOf(' ', CELL_MAX);
    if (sp > CELL_MAX * 0.8) cut = sp;
    parts.push(rem.substring(0, cut).trimEnd());
    rem = rem.substring(cut).trimStart();
  }
  if (rem) parts.push(rem);
  return parts;
}

function getAllParts(sheet, baseIndex) {
  const lastRow = sheet.getLastRow();
  if (lastRow < 2) return "";
  const vals = sheet.getRange(2, COLS.INDEX, lastRow - 1, 2).getValues();
  const baseLower = baseIndex.toLowerCase();
  const map = {};
  for (const row of vals) {
    const idx = String(row[0]);
    if (idx.toLowerCase() === baseLower) { map[0] = String(row[1]); continue; }
    const m = idx.match(/^(.+)\[(\d+)\]$/);
    if (m && m[1].toLowerCase() === baseLower) map[parseInt(m[2]) - 1] = String(row[1]);
  }
  return Object.keys(map).map(Number).sort((a,b)=>a-b).map(k=>map[k]).join(' ');
}

function getEntryDate(sheet, baseIndex) {
  const lastRow = sheet.getLastRow();
  if (lastRow < 2) return "";
  const vals = sheet.getRange(2, COLS.INDEX, lastRow - 1, 3).getValues();
  for (const row of vals) {
    if (String(row[0]).toLowerCase() === baseIndex.toLowerCase()) return String(row[2]);
  }
  return "";
}

function deleteAllParts(sheet, baseIndex) {
  const lastRow = sheet.getLastRow();
  if (lastRow < 2) return 0;
  const baseLower = baseIndex.toLowerCase();
  const vals = sheet.getRange(2, COLS.INDEX, lastRow - 1, 1).getValues();
  let deleted = 0;
  for (let i = vals.length - 1; i >= 0; i--) {
    const idx = String(vals[i][0]);
    const isBase = idx.toLowerCase() === baseLower;
    const isCont = /\[\d+\]$/.test(idx) &&
                   idx.replace(/\[\d+\]$/,'').toLowerCase() === baseLower;
    if (isBase || isCont) { sheet.deleteRow(i + 2); deleted++; }
  }
  return deleted;
}

function countBaseEntries(sheet) {
  const lastRow = sheet.getLastRow();
  if (lastRow < 2) return 0;
  const vals = sheet.getRange(2, COLS.INDEX, lastRow - 1, 1).getValues();
  return vals.filter(r => r[0] && !/\[\d+\]$/.test(String(r[0]))).length;
}

// ── Sheet formatting ──────────────────────────────────────────
function setupSheetHeaders(sheet) {
  sheet.getRange(1, 1, 1, 3).setValues([["Index", "Explanation", "Date"]]);
  const hdr = sheet.getRange(1, 1, 1, 3);
  hdr.setBackground("#1a73e8");
  hdr.setFontColor("#ffffff");
  hdr.setFontWeight("bold");
  hdr.setFontSize(11);
  hdr.setHorizontalAlignment("center");
  sheet.setColumnWidth(1, 200);
  sheet.setColumnWidth(2, 650);
  sheet.setColumnWidth(3, 160);
  sheet.setFrozenRows(1);
  sheet.getRange(1, 1, 1, 3).createFilter();

  // Column 4 থেকে শেষ পর্যন্ত সব hide করো
  const maxCols = sheet.getMaxColumns();
  if (maxCols > 3) {
    sheet.hideColumns(4, maxCols - 3);
  }
}

function styleLastRow(sheet, isContinuation = false) {
  const row   = sheet.getLastRow();
  const bg    = isContinuation ? "#e8f0fe"            // continuation → হালকা নীল
              : row % 2 === 0  ? "#f8f9fa" : "#ffffff";
  sheet.getRange(row, 1, 1, 3).setBackground(bg);
  sheet.getRange(row, 2).setWrap(true);
  // Date column center align
  sheet.getRange(row, 3).setHorizontalAlignment("center");
  sheet.setRowHeight(row, 21);
}

function getTimestamp() {
  return Utilities.formatDate(
    new Date(),
    Session.getScriptTimeZone(),
    "dd/MM/yyyy HH:mm"
  );
}

function json(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

// ── Already তৈরি sheet গুলো fix করতে একবার run করুন ─────────
function fixExistingSheets() {
  const registry = getRegistry();
  let fixed = 0;

  for (const db of registry) {
    let ss;
    try { ss = SpreadsheetApp.openById(db.id); }
    catch (e) { continue; }

    for (const sheet of ss.getSheets()) {
      const maxCols = sheet.getMaxColumns();
      if (maxCols > 3) {
        sheet.hideColumns(4, maxCols - 3);
        fixed++;
      }
    }
  }
  Logger.log(`Fixed ${fixed} sheet(s) — extra columns hidden`);
}

// ── First time setup ──────────────────────────────────────────
// এটা একবার manually run করো → সব database তৈরি হবে
function initialSetup() {
  Logger.log("Creating databases...");
  const reg = getRegistry();
  Logger.log("Done! Databases: " + JSON.stringify(reg.map(r => r.subject)));
}

// ── Test ──────────────────────────────────────────────────────
function testRemember() {
  const r = remember({
    subject: "Physics", chapter: "Physics 1",
    index: "Newton's Laws",
    explanation: "নিউটনের প্রথম সূত্র: কোনো বস্তু স্থির থাকলে স্থিরই থাকবে..."
  });
  Logger.log(JSON.stringify(r));
}

function testRecall() {
  const r = recall({ query: "newton", subject: "Physics" });
  Logger.log(JSON.stringify(r));
}
