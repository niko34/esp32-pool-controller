import os
import sys
import json
import re
from datetime import datetime
from pathlib import Path

from dotenv import load_dotenv
from crewai import Crew

# -----------------------------
# Always run from repo root + ensure imports work
# -----------------------------
REPO_ROOT = Path(__file__).resolve().parents[1]
os.chdir(REPO_ROOT)
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
print(f"[run_feature] CWD = {Path.cwd()}")

from ai.crew.agents import (
    build_llms,
    product_agent,
    architect_agent,
    dev_agent,
    qa_agent,
    accessibility_agent,
    security_agent,
)
from ai.crew.tasks_feature import (
    triage_feature,
    write_ticket,
    design_solution,
    plan_tests,
    a11y_review,
    security_review,
    dev_patch,
)

# Optional (nice-to-have): repo facts for dev (if you already created it)
try:
    from ai.crew.tools.repo_reader import RepoReader  # type: ignore
except Exception:
    RepoReader = None  # tool not present


# -----------------------------
# Helpers: repo context
# -----------------------------
def read_text(path: str, max_chars: int = 12000) -> str:
    p = Path(path)
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="ignore")[:max_chars]


def repo_context() -> str:
    """
    Minimal repo context fed to agents.
    Keep excerpts short to avoid huge prompts.
    """
    parts = []

    for f in ["README.md", "API.md", "SIMULATION_GUIDE.md", "CHANGELOG.md"]:
        txt = read_text(f, 12000)
        if txt.strip():
            parts.append(f"{f}:\n{txt}")

    # Embedded UI bundle (often large/minified; keep small excerpts)
    parts.append("data/index.html (extrait):\n" + read_text("data/index.html", 6000))
    parts.append("data/app.js (extrait):\n" + read_text("data/app.js", 6000))

    return "\n\n---\n\n".join(parts)


def repo_facts() -> str:
    """
    Optional: lightweight repo "facts" to anchor dev patches.
    If RepoReader tool isn't available, returns empty string.
    """
    if RepoReader is None:
        return ""

    rr = RepoReader(".")
    facts = []

    def fmt(title, hits):
        if not hits:
            return f"{title}: (aucun résultat)\n"
        lines = [f"{title}:"]
        for h in hits[:15]:
            lines.append(f"- {h['file']}:{h['line']}  {h['text']}")
        return "\n".join(lines) + "\n"

    facts.append(fmt("Routes (server->on)", rr.search(r"server->on\(")))
    facts.append(fmt("Auth guards (REQUIRE_AUTH)", rr.search(r"REQUIRE_AUTH")))
    facts.append(fmt("Mentions /api/", rr.search(r"/api/")))
    facts.append(fmt("UI fetch()", rr.search(r"\bfetch\(")))

    facts.append("Fichier UI (data/app.js extrait):\n" + rr.read_file("data/app.js", 6000))
    facts.append("Fichier UI (data/index.html extrait):\n" + rr.read_file("data/index.html", 4000))

    return "\n\n---\n\n".join(facts)


# -----------------------------
# Task output extraction (CrewAI versions vary)
# -----------------------------
def task_output_text(task) -> str:
    """
    Tries to extract raw text output from a CrewAI Task across versions.
    """
    out = ""
    try:
        out = task.output.raw  # type: ignore[attr-defined]
    except Exception:
        pass

    if not out:
        try:
            out = str(task.output)  # type: ignore[attr-defined]
        except Exception:
            out = ""

    return out or ""


# -----------------------------
# JSON extraction helpers (triage output)
# -----------------------------
def parse_json_loose(text: str) -> dict:
    """
    Parses JSON even if the model added extra text.
    Strategy:
    - try direct json.loads
    - else find the first {...} block that parses
    """
    text = (text or "").strip()
    if not text:
        raise ValueError("Triage output is empty.")

    # Direct
    try:
        return json.loads(text)
    except Exception:
        pass

    # If there is a ```json ...``` block
    m = re.findall(r"```json\s*(\{.*?\})\s*```", text, flags=re.S)
    if m:
        for candidate in reversed(m):
            try:
                return json.loads(candidate)
            except Exception:
                continue

    # Otherwise, try to locate a JSON object block
    brace_blocks = re.findall(r"(\{.*\})", text, flags=re.S)
    for candidate in brace_blocks:
        candidate = candidate.strip()
        try:
            return json.loads(candidate)
        except Exception:
            continue

    raise ValueError("Could not parse triage JSON output. Output was:\n" + text[:1000])


def normalize_triage(obj: dict) -> dict:
    """
    Validates and normalizes the PO triage JSON.
    Expected keys:
      - ticket_id (str)
      - areas (list[str])
      - iterations (int)
      - deliverables (list[str])
      - constraints (dict)
      - definition_of_done (list[str])
      - notes (str)
    """
    out = dict(obj)

    out.setdefault("ticket_id", f"FEAT-{datetime.now().strftime('%Y%m%d')}-001")
    out.setdefault("areas", [])
    out.setdefault("iterations", 0)
    out.setdefault("deliverables", ["ticket", "design", "tests", "a11y", "security"])
    out.setdefault("constraints", {"backward_compatible": True, "breaking_changes_allowed": False})
    out.setdefault("definition_of_done", [])
    out.setdefault("notes", "")

    # Sanitize types
    if not isinstance(out["areas"], list):
        out["areas"] = []
    out["areas"] = [str(a) for a in out["areas"]]

    try:
        out["iterations"] = int(out["iterations"])
    except Exception:
        out["iterations"] = 0
    out["iterations"] = max(0, min(out["iterations"], 5))  # clamp 0..5

    if not isinstance(out["deliverables"], list):
        out["deliverables"] = ["ticket", "design", "tests", "a11y", "security"]
    out["deliverables"] = [str(d) for d in out["deliverables"]]

    if not isinstance(out["definition_of_done"], list):
        out["definition_of_done"] = []

    if not isinstance(out["constraints"], dict):
        out["constraints"] = {"backward_compatible": True, "breaking_changes_allowed": False}

    return out


def wants(deliverables: list[str], key: str) -> bool:
    return key in set(deliverables or [])


# -----------------------------
# Runner
# -----------------------------
def main(feature_text: str) -> None:
    load_dotenv("ai/.env")

    # LLMs: Haiku for PO/Arch/QA/A11y/Sec, Sonnet for Dev
    llm_haiku, llm_sonnet = build_llms()

    po = product_agent(llm_haiku)
    arch = architect_agent(llm_haiku)
    qa = qa_agent(llm_haiku)
    a11y = accessibility_agent(llm_haiku)
    sec = security_agent(llm_haiku)
    dev = dev_agent(llm_sonnet)

    ctx = repo_context()
    facts = repo_facts()
    if facts:
        ctx_for_dev = ctx + "\n\n---\n\nREPO FACTS:\n" + facts
    else:
        ctx_for_dev = ctx

    out_dir = Path("ai/outputs/features") / datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "run.log").write_text("run started\n", encoding="utf-8")
    print(f"[run_feature] out_dir = {out_dir}")

    # -----------------------------
    # Phase 1: PO triage (decide areas + iterations + deliverables)
    # -----------------------------
    t0 = triage_feature(po, feature_text, ctx)
    crew_triage = Crew(agents=[po], tasks=[t0], verbose=True)

    try:
        triage_result = crew_triage.kickoff()
    except Exception as e:
        (out_dir / "error_triage.txt").write_text(repr(e), encoding="utf-8")
        raise

    triage_text = str(triage_result)
    (out_dir / "00_triage_raw.txt").write_text(triage_text, encoding="utf-8")

    triage_obj = normalize_triage(parse_json_loose(triage_text))
    (out_dir / "00_triage.json").write_text(json.dumps(triage_obj, indent=2, ensure_ascii=False), encoding="utf-8")

    deliverables = triage_obj["deliverables"]
    areas = triage_obj["areas"]
    iterations = triage_obj["iterations"]

    # -----------------------------
    # Phase 2: Build tasks dynamically
    # -----------------------------
    tasks = []
    names = []

    # Always produce ticket/design/tests/a11y/security unless PO explicitly removed them
    # (We still allow PO to remove items via deliverables.)
    if wants(deliverables, "ticket"):
        t1 = write_ticket(po, feature_text, ctx)
        tasks.append(t1)
        names.append("01_ticket")
    else:
        t1 = None

    if wants(deliverables, "design"):
        # needs ticket output if present
        ticket_ref = "{output_of_t1}" if t1 is not None else feature_text
        t2 = design_solution(arch, ticket_ref, ctx)
        tasks.append(t2)
        names.append("02_design")
    else:
        t2 = None

    if wants(deliverables, "tests"):
        ticket_ref = "{output_of_t1}" if t1 is not None else feature_text
        design_ref = "{output_of_t2}" if t2 is not None else ""
        t3 = plan_tests(qa, ticket_ref, design_ref)
        tasks.append(t3)
        names.append("03_test_plan")
    else:
        t3 = None

    if wants(deliverables, "a11y"):
        ticket_ref = "{output_of_t1}" if t1 is not None else feature_text
        design_ref = "{output_of_t2}" if t2 is not None else ""
        t4 = a11y_review(a11y, ticket_ref, design_ref)
        tasks.append(t4)
        names.append("04_a11y")
    else:
        t4 = None

    if wants(deliverables, "security"):
        ticket_ref = "{output_of_t1}" if t1 is not None else feature_text
        design_ref = "{output_of_t2}" if t2 is not None else ""
        t5 = security_review(sec, ticket_ref, design_ref)
        tasks.append(t5)
        names.append("05_security")
    else:
        t5 = None

    # Dev patch iterations, only if PO wants dev_patch and iterations > 0
    # We intentionally run dev AFTER ticket+design are available.
    if wants(deliverables, "dev_patch") and iterations > 0:
        if t1 is None or t2 is None:
            # If PO asked dev_patch, but removed ticket/design, we still need something.
            # We fall back to feature_text and ctx.
            ticket_ref = feature_text if t1 is None else "{output_of_t1}"
            design_ref = "" if t2 is None else "{output_of_t2}"
        else:
            ticket_ref = "{output_of_t1}"
            design_ref = "{output_of_t2}"

        for i in range(1, iterations + 1):
            t_dev_i = dev_patch(
                dev=dev,
                ticket_md=ticket_ref,
                design_md=design_ref,
                repo_context=ctx_for_dev,
                areas=areas,
                iteration_index=i,
                total_iterations=iterations,
            )
            tasks.append(t_dev_i)
            names.append(f"06_dev_patch_iter{i:02d}")

    # If nothing selected (edge case), just dump triage and exit
    if not tasks:
        (out_dir / "result_all.md").write_text(
            "# No tasks executed\n\nPO triage removed all deliverables.\n",
            encoding="utf-8",
        )
        print("⚠️ Aucun livrable demandé par le PO (deliverables vide).")
        return

    # -----------------------------
    # Phase 3: Run crew
    # -----------------------------
    agents = [po, arch, qa, a11y, sec, dev]
    crew = Crew(agents=agents, tasks=tasks, verbose=True)

    try:
        result = crew.kickoff()

        # Final output (often equals last task)
        (out_dir / "result.md").write_text(str(result), encoding="utf-8")

        # Save each task output + combined
        all_parts = []
        for t, name in zip(tasks, names):
            out = task_output_text(t).strip()
            (out_dir / f"{name}.md").write_text(out, encoding="utf-8")
            all_parts.append(f"# {name}\n\n{out}")

        (out_dir / "result_all.md").write_text("\n\n---\n\n".join(all_parts), encoding="utf-8")
        print(f"✅ Écrit: {out_dir/'result_all.md'} + fichiers par task")

    except Exception as e:
        (out_dir / "error.txt").write_text(repr(e), encoding="utf-8")
        print(f"❌ Erreur: {e!r} (voir {out_dir/'error.txt'})")
        raise

    print(f"\n✅ Ticket pack généré : {out_dir}/result_all.md\n")


if __name__ == "__main__":
    feature = " ".join(sys.argv[1:]).strip()
    if not feature:
        print('Usage: python -m ai.run_feature "Décrire la feature"')
        raise SystemExit(1)
    main(feature)