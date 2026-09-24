// Builds the ASCII-only JSX from the UTF-8 source.
// Korean runs inside double-quoted strings become K("hex,hex,...") calls, so the file
// survives any transfer/encoding path (no BOM, no \u escapes needed).
const fs = require('fs');
// usage: node escape.cjs [src.jsx] [out.jsx]  (defaults: SC01 v03)
const inPath = process.argv[2] || __dirname + '/BUILD_SC01_v03.src.jsx';
const outPath = process.argv[3] || __dirname + '/../BUILD_SC01_v03.jsx';
let src = fs.readFileSync(inPath, 'utf8');
src = src.replace(/"((?:[^"\\\n]|\\.)*)"/g, (m, body) => {
  if (!/[^\x00-\x7f]/.test(body)) return m;
  const parts = body.split(/([^\x00-\x7f]+)/).filter(p => p !== '');
  return '(' + parts.map(p => /[^\x00-\x7f]/.test(p)
    ? 'K("' + Array.from(p).map(c => c.charCodeAt(0).toString(16)).join(',') + '")'
    : '"' + p + '"').join(' + ') + ')';
});
if (/[^\x00-\x7f]/.test(src)) throw new Error('non-ASCII left outside double-quoted strings');
const helper = '    // decode Korean stored as hex code points (keeps this file pure ASCII)\n' +
  '    function K(h) { var a = h.split(","), s = "", i; for (i = 0; i < a.length; i++) { s += String.fromCharCode(parseInt(a[i], 16)); } return s; }\n';
src = src.replace('    var log = [];\n', helper + '\n    var log = [];\n');
fs.writeFileSync(outPath, src);
