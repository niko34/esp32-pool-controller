from dotenv import load_dotenv
from crewai import Agent, Task, Crew, LLM

load_dotenv("ai/.env")

llm = LLM(
    provider="anthropic",
    model="claude-sonnet-4-5",
    temperature=0.2,
)

agent = Agent(
    role="Smoke Test Agent",
    goal="Répondre en une phrase.",
    backstory="Agent minimal pour valider la config LLM.",
    llm=llm,
    verbose=True,
)

task = Task(
    description="Dis bonjour en une phrase (en français).",
    agent=agent,
    expected_output="Une phrase de salutation en français."
)

crew = Crew(agents=[agent], tasks=[task], verbose=True)
result = crew.kickoff()
print("\nRESULT:\n", result)