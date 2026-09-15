"""Categorical reference KL in log space, with a shared legal-action support."""
import torch


def masked_reference_kl(reference_logits, policy_logits, legal):
    """Return per-row KL(reference || policy), without testing rounded q==0.

    Finite legal logits imply mathematically positive probabilities even when
    their float32 softmax underflows. Illegal actions contribute exactly zero.
    The reference may be frozen by the caller; current-policy gradients are
    retained. Half-precision inputs are accumulated in float32.
    """
    if reference_logits.shape!=policy_logits.shape or legal.shape!=policy_logits.shape:
        raise ValueError('KL logits and action masks must have identical shapes')
    if legal.dtype!=torch.bool or policy_logits.ndim<1 or policy_logits.shape[-1]==0:
        raise ValueError('KL requires a boolean nonempty action mask')
    if not legal.any(-1).all():raise ValueError('KL row has no legal actions')
    if not torch.isfinite(reference_logits[legal]).all() or not torch.isfinite(policy_logits[legal]).all():
        raise ValueError('KL legal logits must be finite')
    dtype=torch.promote_types(reference_logits.dtype,policy_logits.dtype)
    if dtype in (torch.float16,torch.bfloat16):dtype=torch.float32
    p=reference_logits.to(dtype).masked_fill(~legal,float('-inf')).log_softmax(-1)
    q=policy_logits.to(dtype).masked_fill(~legal,float('-inf')).log_softmax(-1)
    difference=p.masked_fill(~legal,0)-q.masked_fill(~legal,0)
    return (p.exp()*difference).sum(-1)
