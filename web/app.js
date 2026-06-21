const API = '';
let schemaFields = [];

document.querySelectorAll('.nav-btn').forEach(btn => {
  btn.addEventListener('click', () => switchView(btn.dataset.view));
});
function switchView(name) {
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  document.querySelector(`.nav-btn[data-view="${name}"]`).classList.add('active');
  document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
  document.getElementById(`view-${name}`).classList.add('active');
  if (name === 'collections' || name === 'dashboard') refreshCollections();
  if (name === 'monitor') refreshMonitor();
}

document.getElementById('sr-mode')?.addEventListener('change', function() {
  const isHybrid = this.value === 'hybrid';
  const isText = this.value === 'text';
  document.getElementById('sr-txt-row').style.display = (isHybrid || isText) ? '' : 'none';
  document.getElementById('sr-alpha').style.display = isHybrid ? '' : 'none';
  document.getElementById('sr-alpha-label').style.display = isHybrid ? '' : 'none';
  document.getElementById('sr-vec').parentElement.style.display = isText ? 'none' : '';
});

async function api(method, path, body) {
  const opts = { method, headers: {'Content-Type':'application/json'} };
  if (body !== undefined) opts.body = JSON.stringify(body);
  const res = await fetch(`${API}${path}`, opts);
  const text = await res.text();
  try { return { ok: res.ok, data: JSON.parse(text), status: res.status }; }
  catch(e) { return { ok: res.ok, data: text, status: res.status }; }
}
function esc(s) { return s == null ? '' : String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

// ======================== DEMO ========================
async function loadDemo() {
  const btn = document.querySelector('#demo-status').previousElementSibling;
  btn.disabled = true; btn.textContent = 'Loading...';
  document.getElementById('demo-status').textContent = '';
  try {
    const r1 = await api('POST', '/collections', {
      name:'products', metric:'cosine', enable_index:true, index_type:'hnsw',
      schema:{ fields:[
        {name:'category',type:'string',indexable:true},
        {name:'price',type:'float'},
        {name:'name',type:'text'},
        {name:'rating',type:'float'}
      ]}
    });
    if (!r1.ok && !r1.data?.error?.includes('already exists')) throw new Error(r1.data?.error);
    const items = [
      {name:'Wireless Mouse',cat:'electronics',price:29.99,rating:4.5,vec:[0.15,0.72,0.33,0.50]},
      {name:'Running Shoes',cat:'sports',price:89.99,rating:4.2,vec:[0.88,0.12,0.45,0.30]},
      {name:'Coffee Maker',cat:'home',price:49.99,rating:4.7,vec:[0.45,0.55,0.78,0.20]},
      {name:'USB-C Hub',cat:'electronics',price:39.99,rating:4.3,vec:[0.22,0.68,0.40,0.55]},
      {name:'Yoga Mat',cat:'sports',price:19.99,rating:4.0,vec:[0.75,0.25,0.30,0.45]},
      {name:'Desk Lamp',cat:'home',price:35.99,rating:4.6,vec:[0.35,0.60,0.70,0.25]},
      {name:'Bluetooth Speaker',cat:'electronics',price:59.99,rating:4.4,vec:[0.18,0.75,0.38,0.52]},
      {name:'Water Bottle',cat:'sports',price:14.99,rating:4.1,vec:[0.80,0.18,0.28,0.48]},
      {name:'Throw Blanket',cat:'home',price:24.99,rating:4.8,vec:[0.40,0.52,0.75,0.22]},
      {name:'Laptop Stand',cat:'electronics',price:45.99,rating:4.3,vec:[0.20,0.70,0.35,0.53]},
    ];
    const docs = items.map(p => ({ id:p.name.toLowerCase().replace(/\s/g,'_'), vector:p.vec, metadata:{ name:p.name, category:p.cat, price:p.price, rating:p.rating } }));
    await api('POST', '/collections/products/batch-upsert', { documents: docs });
    document.getElementById('demo-status').textContent = `✅ 10 products loaded with HNSW index`;
    refreshCollections();
  } catch(e) { document.getElementById('demo-status').textContent = '❌ '+e.message; }
  btn.disabled = false; btn.textContent = 'Load Demo Data';
}

// ======================== COLLECTIONS ========================
async function refreshCollections() {
  const r = await api('GET', '/collections');
  const cols = r.ok ? r.data : [];
  document.getElementById('dash-collections').textContent = cols.length;
  document.getElementById('dash-vectors').textContent = cols.reduce((s,c) => s+(c.count||0), 0).toLocaleString();
  document.getElementById('dash-indexed').textContent = cols.filter(c=>c.index_type).length+'/'+cols.length;
  document.getElementById('mon-collections').textContent = cols.length;
  document.getElementById('mon-vectors').textContent = cols.reduce((s,c) => s+(c.count||0), 0).toLocaleString();
  document.getElementById('mon-indexed').textContent = cols.filter(c=>c.index_type).length+'/'+cols.length;

  const tb = document.getElementById('collections-table');
  tb.innerHTML = cols.length ? cols.map(c => `<tr><td><strong>${esc(c.name)}</strong></td><td>${c.count}</td><td>${c.dimension||'—'}</td><td>${esc(c.metric)}</td><td>${c.index_type?'<span class="badge">'+esc(c.index_type)+'</span>':'—'}</td><td>${c.schema?'✅':'—'}</td></tr>`).join('') : '<tr><td colspan="6">No collections</td></tr>';

  ['da-collection','sr-collection','rc-col','ix-col'].forEach(id => {
    const sel = document.getElementById(id);
    if (!sel) return;
    sel.innerHTML = cols.map(c => `<option value="${esc(c.name)}">${esc(c.name)} (${c.count})</option>`).join('');
  });
}

// ======================== SCHEMA ========================
function addField() {
  const name = document.getElementById('sf-name').value.trim();
  if (!name) return alert('Name required');
  if (schemaFields.find(f=>f.name===name)) return alert('Duplicate');
  schemaFields.push({name, type:document.getElementById('sf-type').value, indexable:document.getElementById('sf-indexable').checked});
  document.getElementById('sf-name').value = ''; document.getElementById('sf-indexable').checked = false;
  document.getElementById('sc-fields-body').innerHTML = schemaFields.map((f,i) => `<tr><td>${esc(f.name)}</td><td><span class="badge">${esc(f.type)}</span></td><td>${f.indexable?'✅':'—'}</td><td><button class="danger" onclick="removeField(${i})" style="padding:0.15rem 0.4rem;font-size:0.75rem">✕</button></td></tr>`).join('') || '<tr><td colspan="3">Add fields</td></tr>';
}
function removeField(i) { schemaFields.splice(i,1); document.getElementById('sc-fields-body').innerHTML = schemaFields.length ? schemaFields.map((f,i) => `<tr>...`).join('') : '<tr><td colspan="3">Add fields</td></tr>'; }
async function createWithSchema() {
  const name = document.getElementById('sc-name').value.trim();
  if (!name) return alert('Name required');
  const body = {name, metric:document.getElementById('sc-metric').value, enable_index:document.getElementById('sc-index').checked};
  if (schemaFields.length) body.schema = {fields:schemaFields.map(f=>({name:f.name,type:f.type,indexable:f.indexable}))};
  const r = await api('POST', '/collections', body);
  document.getElementById('sc-result').textContent = JSON.stringify(r.data,null,2);
  if (r.ok) { schemaFields=[]; document.getElementById('sc-fields-body').innerHTML='<tr><td colspan="3">Add fields</td></tr>'; refreshCollections(); }
}

// ======================== DATA ========================
async function loadDataDocs() {
  const col = document.getElementById('da-collection').value;
  if (!col) return;
  const r = await api('GET', '/collections');
  const c = r.data?.find(x=>x.name===col);
  document.getElementById('da-stats').textContent = c ? `Count: ${c.count} | Dim: ${c.dimension||'?'} | Metric: ${c.metric}` : '';
}
async function doUpsert() {
  const col = document.getElementById('da-collection').value, id = document.getElementById('da-id').value.trim();
  const vec = document.getElementById('da-vec').value.trim().split(',').map(s=>parseFloat(s.trim()));
  const meta = document.getElementById('da-meta').value.trim();
  if (!col||!id||vec.some(isNaN)) return alert('Collection, ID, and vector required');
  let metadata = {};
  if (meta) { try{metadata=JSON.parse(meta)}catch(e){return alert('Invalid metadata JSON')} }
  const r = await api('POST', `/collections/${encodeURIComponent(col)}/upsert`, {id,vector:vec,metadata});
  document.getElementById('da-result').textContent = JSON.stringify(r.data,null,2);
  if (r.ok) { loadDataDocs(); refreshCollections(); }
}
async function doBatchUpsert() {
  const col = document.getElementById('da-collection').value, raw = document.getElementById('da-batch').value.trim();
  if (!col||!raw) return alert('Required');
  let docs; try{docs=JSON.parse(raw)}catch(e){return alert('Invalid JSON: '+e.message)}
  const r = await api('POST', `/collections/${encodeURIComponent(col)}/batch-upsert`, {documents:docs});
  document.getElementById('da-batch-result').textContent = JSON.stringify(r.data,null,2);
  if (r.ok) { loadDataDocs(); refreshCollections(); }
}

// ======================== SEARCH ========================
async function doSearch() {
  const col = document.getElementById('sr-collection').value, mode = document.getElementById('sr-mode').value;
  const k = parseInt(document.getElementById('sr-k').value)||5;
  if (!col) return alert('Select collection');
  let filter = {};
  const fs = document.getElementById('sr-filter').value.trim();
  if (fs) { try{filter=JSON.parse(fs)}catch(e){return alert('Invalid filter JSON')} }

  let r;
  if (mode==='text') {
    const txt = document.getElementById('sr-txt').value.trim();
    if (!txt) return alert('Text query required');
    r = await api('POST', `/collections/${encodeURIComponent(col)}/hybrid-search`, {query:[],text_query:txt,k,alpha:0,filter});
  } else {
    const vec = document.getElementById('sr-vec').value.trim().split(',').map(s=>parseFloat(s.trim()));
    if (vec.some(isNaN)) return alert('Invalid vector');
    if (mode==='hybrid') {
      const txt = document.getElementById('sr-txt').value.trim();
      if (!txt) return alert('Text query required');
      r = await api('POST', `/collections/${encodeURIComponent(col)}/hybrid-search`, {query:vec,text_query:txt,k,alpha:parseFloat(document.getElementById('sr-alpha').value)||0.5,filter});
    } else {
      r = await api('POST', `/collections/${encodeURIComponent(col)}/search`, {query:vec,k,filter});
    }
  }
  const results = r.data.results||[];
  document.getElementById('sr-results').style.display = '';
  document.getElementById('sr-stats').textContent = `${results.length} results | ${((r.data.total_time_ns||0)/1e6).toFixed(2)}ms${r.data.index_used ? ' | '+r.data.index_used : ''}`;
  if (!results.length) { document.getElementById('sr-body').innerHTML = '<tr><td colspan="4">No results</td></tr>'; document.getElementById('sr-bars').innerHTML = ''; return; }
  document.getElementById('sr-body').innerHTML = results.map((r,i)=>`<tr><td>${i+1}</td><td><strong>${esc(r.id)}</strong></td><td>${r.score.toFixed(4)}</td><td style="font-size:0.82rem">${r.document?.metadata?esc(JSON.stringify(r.document.metadata)):'—'}</td></tr>`).join('');
  const ms=Math.max(...results.map(r=>r.score)), ns=Math.min(...results.map(r=>r.score)), rg=ms-ns||1;
  document.getElementById('sr-bars').innerHTML = '<div style="font-size:0.85rem;font-weight:600;margin-bottom:0.5rem">Score Distribution</div>'+results.map(r=>`<div class="score-bar"><span class="score-bar-label">${esc(r.id)}</span><div class="score-bar-fill" style="width:${Math.max((r.score-ns)/rg*100,2)}%"></div><span>${r.score.toFixed(4)}</span></div>`).join('');
}

// ======================== MONITOR ========================
async function refreshMonitor() { await refreshCollections(); }
async function doRecall() {
  const col = document.getElementById('rc-col').value, vec = document.getElementById('rc-vec').value.trim().split(',').map(s=>parseFloat(s.trim()));
  const k = parseInt(document.getElementById('rc-k').value)||10;
  if (!col||vec.some(isNaN)) return alert('Required');
  const r = await api('POST', `/collections/${encodeURIComponent(col)}/recall`, {query:vec,k});
  if (!r.ok) return alert(JSON.stringify(r.data));
  const d = r.data;
  document.getElementById('rc-metrics').style.display = '';
  document.getElementById('rc-recall-val').textContent = (d.recall*100).toFixed(1)+'%';
  document.getElementById('rc-ann-lat').textContent = (d.ann_time_ns/1e3).toFixed(1);
  document.getElementById('rc-bf-lat').textContent = (d.bf_time_ns/1e3).toFixed(1);
  document.getElementById('rc-speedup-val').textContent = d.bf_time_ns>0&&d.ann_time_ns>0?(d.bf_time_ns/d.ann_time_ns).toFixed(1)+'x':'—';
  document.getElementById('rc-ix-status').textContent = d.ann_searched?`${d.index_type} | ${d.ann_candidates}/${d.bf_candidates}`:'No index';
  document.getElementById('rc-compare').style.display = '';
  document.getElementById('rc-ann-label').textContent = `(${(d.ann_time_ns/1e3).toFixed(1)}µs)`;
  const bfSet = new Set((d.bf_results||[]).map(x=>x.id));
  document.getElementById('rc-ann-body').innerHTML = (d.ann_results||[]).map((x,i)=>{const m=bfSet.has(x.id);return `<tr class="${m?'match-hit':'match-miss'}"><td>${i+1}</td><td>${esc(x.id)}</td><td>${x.score.toFixed(4)}</td><td>${m?'✅':'❌'}</td></tr>`;}).join('')||'<tr><td colspan="4">No results</td></tr>';
  document.getElementById('rc-bf-body').innerHTML = (d.bf_results||[]).map((x,i)=>`<tr><td>${i+1}</td><td>${esc(x.id)}</td><td>${x.score.toFixed(4)}</td></tr>`).join('')||'<tr><td colspan="3">No results</td></tr>';
}

// ======================== INDEX ========================
async function refreshIndex() {
  const col = document.getElementById('ix-col').value;
  if (!col) { document.getElementById('ix-status-txt').textContent = 'Select a collection'; return; }
  const r = await api('GET', `/collections/${encodeURIComponent(col)}/index`);
  document.getElementById('ix-status-txt').textContent = r.ok&&r.data.index_type ? `🟢 ${r.data.index_type}` : '🔴 No index';
  document.getElementById('ix-info').textContent = r.ok ? JSON.stringify(r.data,null,2) : '';
}
async function doBuildIndex(type) {
  const col = document.getElementById('ix-col').value;
  if (!col) return alert('Select collection');
  const r = await api('POST', `/collections/${encodeURIComponent(col)}/index`, {action:'build',index_type:type});
  document.getElementById('ix-info').textContent = JSON.stringify(r.data,null,2);
  if (r.ok) refreshIndex();
}
async function doDropIndex() {
  const col = document.getElementById('ix-col').value;
  if (!col||!confirm('Drop index?')) return;
  const r = await api('POST', `/collections/${encodeURIComponent(col)}/index`, {action:'drop'});
  document.getElementById('ix-info').textContent = JSON.stringify(r.data,null,2);
  if (r.ok) refreshIndex();
}
document.getElementById('ix-col')?.addEventListener('change', refreshIndex);

refreshCollections();
