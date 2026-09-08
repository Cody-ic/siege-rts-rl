import sys
from pathlib import Path
import unittest
import torch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'train'))
from stable_kl import masked_reference_kl


class StableKLTests(unittest.TestCase):
    def test_matches_library_values_and_gradients_on_ordinary_logits(self):
        torch.manual_seed(41)
        p=torch.randn(20,13,dtype=torch.float64)
        q=torch.randn(20,13,dtype=torch.float64,requires_grad=True)
        mask=torch.rand(20,13)>.4;mask[:,0]=True
        actual=masked_reference_kl(p,q,mask)
        expected=torch.distributions.kl_divergence(
            torch.distributions.Categorical(logits=p.masked_fill(~mask,-torch.inf)),
            torch.distributions.Categorical(logits=q.masked_fill(~mask,-torch.inf)))
        torch.testing.assert_close(actual,expected)
        actual_gradient=torch.autograd.grad(actual.sum(),q,retain_graph=True)[0]
        expected_gradient=torch.autograd.grad(expected.sum(),q)[0]
        torch.testing.assert_close(actual_gradient,expected_gradient)

    def test_underflow_remains_finite_with_correct_gradient(self):
        p=torch.tensor([[0.,-1000.,17.]])
        q=torch.tensor([[-1000.,0.,-19.]],requires_grad=True)
        mask=torch.tensor([[True,True,False]])
        loss=masked_reference_kl(p,q,mask)
        torch.testing.assert_close(loss,torch.tensor([1000.]))
        loss.sum().backward()
        torch.testing.assert_close(q.grad,torch.tensor([[-1.,1.,0.]]))

    def test_masked_infinities_and_single_legal_action(self):
        p=torch.tensor([[4.,-torch.inf,float('nan')]])
        q=torch.tensor([[7.,-torch.inf,float('nan')]],requires_grad=True)
        mask=torch.tensor([[True,False,False]])
        loss=masked_reference_kl(p,q,mask)
        self.assertEqual(loss.item(),0)
        loss.sum().backward()
        torch.testing.assert_close(q.grad,torch.zeros_like(q))

    def test_half_precision_promotes_accumulation(self):
        p=torch.tensor([[1000.,-1000.]],dtype=torch.float16)
        q=-p
        result=masked_reference_kl(p,q,torch.ones_like(p,dtype=torch.bool))
        self.assertEqual(result.dtype,torch.float32)
        self.assertEqual(result.item(),2000)

    def test_rejects_empty_support_shape_and_nonfinite_legal_logits(self):
        p=torch.zeros(2,3)
        for q,mask in ((p,torch.zeros_like(p,dtype=torch.bool)),
                       (p[:,:2],torch.ones_like(p,dtype=torch.bool)),
                       (p+torch.inf,torch.ones_like(p,dtype=torch.bool))):
            with self.assertRaises(ValueError):masked_reference_kl(p,q,mask)


if __name__=='__main__':unittest.main()
