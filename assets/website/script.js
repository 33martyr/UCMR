const boxes = [...document.querySelectorAll('[data-completes]')];
const labels = {
   0: 'Start with the Collection setup',
   1: 'Collection is installed',
   2: 'PS4 Collection and update 1.02 are ready',
   3: 'PS3 files and EBOOT are ready',
   4: 'Import complete — sign in to RPCN',
   5: 'RPCN ready — start multiplayer'
};
const topBar = document.getElementById('readiness-top');
const floatingBar = document.getElementById('readiness-floating');
const confettiCanvas = document.getElementById('confetti');
const confettiContext = confettiCanvas && confettiCanvas.getContext('2d');
let confettiPieces = [];
let confettiFrame = 0;

function drawConfetti() {
  confettiContext.clearRect(0, 0, innerWidth, innerHeight);
  confettiPieces = confettiPieces.filter((piece) => piece.life > 0);
  for (const piece of confettiPieces) {
    piece.x += piece.vx;
    piece.y += piece.vy;
    piece.vy += 0.22;
    piece.vx *= 0.99;
    piece.angle += piece.spin;
    piece.life--;
    confettiContext.save();
    confettiContext.translate(piece.x, piece.y);
    confettiContext.rotate(piece.angle);
    confettiContext.globalAlpha = Math.min(1, piece.life / 30);
    confettiContext.fillStyle = piece.color;
    confettiContext.fillRect(-piece.size / 2, -piece.size / 2, piece.size, piece.size * 1.6);
    confettiContext.restore();
  }
  if (confettiPieces.length) {
    confettiFrame = requestAnimationFrame(drawConfetti);
  } else {
    confettiFrame = 0;
    confettiContext.clearRect(0, 0, innerWidth, innerHeight);
  }
}

function launchConfetti() {
  if (!confettiContext || (window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches)) return;
  const ratio = Math.min(devicePixelRatio || 1, 2);
  confettiCanvas.width = Math.floor(innerWidth * ratio);
  confettiCanvas.height = Math.floor(innerHeight * ratio);
  confettiContext.setTransform(ratio, 0, 0, ratio, 0, 0);
  const colors = ['#efb634', '#ffd56c', '#f4f0e8', '#66c871', '#d7c7aa'];
  for (let i = 0; i < 80; ++i) {
    confettiPieces.push({
      x: innerWidth * (0.35 + Math.random() * 0.3),
      y: innerHeight + 12,
      vx: (Math.random() - 0.5) * 7,
      vy: -(8 + Math.random() * 8),
      size: 4 + Math.random() * 5,
      angle: Math.random() * Math.PI,
      spin: (Math.random() - 0.5) * 0.3,
      color: colors[i % colors.length],
      life: 90 + Math.random() * 35
    });
  }
  if (!confettiFrame) confettiFrame = requestAnimationFrame(drawConfetti);
}

function updateProgress() {
  const complete = boxes.filter((box) => box.checked).length;
  const finished = boxes.length > 0 && complete === boxes.length;
  document.querySelectorAll('[data-readiness-copy]').forEach((copy) => {
    copy.textContent = labels[complete];
  });
  document.querySelectorAll('.readiness').forEach((bar) => {
    bar.classList.toggle('is-complete', finished);
    bar.querySelectorAll('.milestones li').forEach((item, index) => {
      item.classList.toggle('done', index < complete);
    });
  });
  localStorage.setItem('uncharted-multiplayer-guide', JSON.stringify(boxes.map((box) => box.checked)));
  if (finished && previousComplete < boxes.length) launchConfetti();
  previousComplete = complete;
}

function syncFloatingBar() {
  if (!topBar || !floatingBar) return;
  floatingBar.classList.toggle('is-visible', topBar.getBoundingClientRect().bottom <= 0);
}

try {
  const saved = JSON.parse(localStorage.getItem('uncharted-multiplayer-guide'));
  if (Array.isArray(saved)) boxes.forEach((box, index) => { box.checked = Boolean(saved[index]); });
} catch { /* First visit or unavailable storage. */ }

let previousComplete = boxes.filter((box) => box.checked).length;
boxes.forEach((box) => box.addEventListener('change', updateProgress));
updateProgress();
syncFloatingBar();

if ('IntersectionObserver' in window && topBar && floatingBar) {
  new IntersectionObserver(([entry]) => {
    floatingBar.classList.toggle('is-visible', !entry.isIntersecting && entry.boundingClientRect.bottom <= 0);
  }, { threshold: 0 }).observe(topBar);
} else {
  window.addEventListener('scroll', syncFloatingBar, { passive: true });
}
