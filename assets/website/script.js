const boxes = [...document.querySelectorAll('[data-completes]')];
const labels = {
   0: 'Start with the Collection setup',
   1: 'Collection is installed',
   2: 'CUSA02320 and update 1.02 are ready',
   3: 'PS3 files and EBOOT are ready',
   4: 'Import complete — start multiplayer',
   5: 'Ready for multiplayer and RPCN'
};

function updateProgress() {
  const complete = boxes.filter((box) => box.checked).length;
  document.getElementById('readiness-copy').textContent = labels[complete];
  document.querySelectorAll('.milestones li').forEach((item, index) => {
    item.classList.toggle('done', index < complete);
  });
  localStorage.setItem('uncharted-multiplayer-guide', JSON.stringify(boxes.map((box) => box.checked)));
}

try {
  const saved = JSON.parse(localStorage.getItem('uncharted-multiplayer-guide'));
  if (Array.isArray(saved)) boxes.forEach((box, index) => { box.checked = Boolean(saved[index]); });
} catch { /* First visit or unavailable storage. */ }

boxes.forEach((box) => box.addEventListener('change', updateProgress));
updateProgress();
