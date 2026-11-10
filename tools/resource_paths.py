"""Canonical paths for project-owned VM resources."""
from pathlib import Path


def resource_path(project, path):
    project = Path(project).resolve()
    resources = (project / "resources").resolve()
    if not resources.is_relative_to(project) or resources == project:
        raise ValueError("Project resources must remain within the project")
    candidate = Path(path).resolve()
    if candidate == resources or not candidate.is_relative_to(resources):
        raise ValueError("VM data must remain inside the project resources directory")
    return candidate


def image_root(project):
    return resource_path(project, Path(project) / "resources/images")


def instance_root(project):
    return resource_path(project, Path(project) / "resources/vm")


def dependency_root(project):
    return resource_path(project, Path(project) / "resources/dependencies")
