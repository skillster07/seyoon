// Stricter dry run: enums only expose members documented in the AE Scripting Guide,
// and setting blendingMode/maskMode/autoOrient/justification to undefined throws (like AE does).
const fs=require('fs');
function mk(path){const f=function(){return mk(path+'()')};return new Proxy(f,{get(t,k){if(k===Symbol.toPrimitive)return ()=>NaN;if(k==='toString')return ()=>path;return mk(path+'.'+String(k));},set(t,k,v){if(k==='expression'){(globalThis.__EXPR=globalThis.__EXPR||[]).push([path,v]);}if(['blendingMode','maskMode','autoOrient','justification'].includes(k)&&v===undefined)throw new Error('Unable to set "'+k+'". Value is undefined. ('+path+')');return true;},apply(){return mk(path+'()');}});}
function en(names){const o={};names.forEach((n,i)=>o[n]=5000+i);return o;}
const g=globalThis;
g.BlendingMode=en(['ADD','ALPHA_ADD','CLASSIC_COLOR_BURN','CLASSIC_COLOR_DODGE','CLASSIC_DIFFERENCE','COLOR','COLOR_BURN','COLOR_DODGE','DANCING_DISSOLVE','DARKEN','DARKER_COLOR','DIFFERENCE','DISSOLVE','DIVIDE','EXCLUSION','HARD_LIGHT','HARD_MIX','HUE','LIGHTEN','LIGHTER_COLOR','LINEAR_BURN','LINEAR_DODGE','LINEAR_LIGHT','LUMINESCENT_PREMUL','LUMINOSITY','MULTIPLY','NORMAL','OVERLAY','PIN_LIGHT','SATURATION','SCREEN','SILHOUETE_ALPHA','SILHOUETTE_LUMA','SOFT_LIGHT','STENCIL_ALPHA','STENCIL_LUMA','SUBTRACT','VIVID_LIGHT']);
g.MaskMode=en(['NONE','ADD','SUBTRACT','INTERSECT','LIGHTEN','DARKEN','DIFFERENCE']);
g.AutoOrientType=en(['ALONG_PATH','CAMERA_OR_POINT_OF_INTEREST','CHARACTERS_TOWARD_CAMERA','NO_AUTO_ORIENT']);
g.ParagraphJustification=en(['CENTER_JUSTIFY','FULL_JUSTIFY_LASTLINE_CENTER','FULL_JUSTIFY_LASTLINE_FULL','FULL_JUSTIFY_LASTLINE_LEFT','FULL_JUSTIFY_LASTLINE_RIGHT','LEFT_JUSTIFY','RIGHT_JUSTIFY']);
g.PropertyType=en(['PROPERTY','INDEXED_GROUP','NAMED_GROUP']);
g.app=mk('app');g.Shape=function(){};g.File=function(){return mk('File')};g.$={fileName:'/x/BUILD.jsx'};g.alert=m=>console.log('ALERT:\n'+m);
const src=fs.readFileSync(process.argv[2],'utf8');
// report every enum member referenced that is not documented
const bad=[...src.matchAll(/\b(BlendingMode|MaskMode|AutoOrientType|ParagraphJustification|PropertyType)\.([A-Z_]+)/g)].filter(m=>!(m[2] in g[m[1]])).map(m=>m[0]);
console.log('undocumented enum refs:', bad.length?bad.join(', '):'none');
eval(src);

(function(){let n=0;(globalThis.__EXPR||[]).forEach(([p,e])=>{try{new Function(e);}catch(err){n++;console.log('EXPR SYNTAX ERROR at',p,'\n',e,'\n',err.message);}});console.log('expressions checked:',(globalThis.__EXPR||[]).length,'syntax errors:',n);})();