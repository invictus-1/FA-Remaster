"""Portable engine test runner. Platform decisions belong in adapters, not tests."""
from .runner import Runner, TestCase, TestResult
from .cases import TestStage

__all__ = ['Runner', 'TestCase', 'TestStage', 'TestResult']
