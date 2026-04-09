<?php

namespace App\Services;

class User
{
    public string $id;
    public string $name;

    public function __construct(string $id, string $name)
    {
        $this->id = $id;
        $this->name = $name;
    }
}

interface UserRepository
{
    public function findById(string $id): ?User;
}

class UserService implements UserRepository
{
    public function findById(string $id): ?User
    {
        return new User($id, 'Ada');
    }

    public function save(User $user): void
    {
        // no-op
    }
}
